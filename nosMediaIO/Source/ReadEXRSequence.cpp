// Copyright MediaZ Teknoloji A.S. All Rights Reserved.

#include <Nodos/PluginHelpers.hpp>
#include <nosVulkanSubsystem/Helpers.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "ExrDecode.h"

namespace nos::mediaio
{

using FileVector = flatbuffers::Vector<flatbuffers::Offset<flatbuffers::String>>;

// Frames kept behind the playhead before eviction, so a just-shown frame is never freed while
// the engine may still reference it and short back-scrubs stay warm.
static constexpr int KEEP_BEHIND = 2;
static constexpr int MAX_AHEAD = 128;
// A frame whose decode failed is retried this long after the failure, so transient errors
// (file still being written, momentary I/O hiccup) self-heal without hammering a bad file.
static constexpr auto FAILED_RETRY_DELAY = std::chrono::milliseconds(500);

// A cached frame. Workers fill Frame (CPU pixels) in parallel; the execute thread uploads it to
// the GPU the first time it is shown, then drops the CPU copy. Workers never touch Vulkan.
struct CacheEntry
{
	enum class State { Decoding, Ready, Failed };
	State St = State::Decoding;
	ExrFrame Frame;
	std::optional<vkss::Resource> Color, Depth;
	std::optional<nos::Buffer> Metadata;
	bool HasDepth = false, Uploaded = false;
	std::string Error;                              // last decode error (State::Failed)
	std::chrono::steady_clock::time_point FailedAt; // when it failed, for retry pacing
};

// Plays an OpenEXR image sequence from a list of files. A pool of worker threads - one per
// physical core, decode throughput flattens beyond that (memory-bandwidth bound) - reads and
// decodes whole frames ahead of Index in parallel; the execute thread uploads and hands out the
// frame for Index. The slow per-frame decode is done ahead of the playhead, so playback stays
// smooth as long as the workers keep up. Each frame is decoded and uploaded once.
//
// Index is taken modulo the file count and the prefetch window wraps around the end of the
// sequence, so a free-running upstream counter loops the clip without a stutter at the seam:
// while the tail plays, the head frames are already being decoded again.
//
// TODO: This node could be decomposed into a graph - index fan-out scatter -> N single-frame
// DecodeEXR instances -> reordering gather -> BoundedQueue -> upload - once scatter/gather
// nodes exist, which would make the read/decode/prefetch machinery visible and reusable
// instead of hidden in here. That covers forward-only (looping) playback; what a FIFO graph
// still would not give is the keyed-cache semantics (instant scrub, direction reversal,
// hold-last-frame, decode-once on hold, failed-frame retry), which turn into a full pipeline
// flush + refill. Fixing those too takes two more nodes: a KeyedCache (index-addressed store
// with an eviction window and hold-last - scrubs then serve resident frames instantly, in any
// order) and a PrefetchPlanner (this node's Claim logic: turns the Index stream into
// nearest-first, ring-wrapped, direction-aware decode requests for missing keys and re-issues
// failures). The crux is that planner and cache need a feedback edge (requests depend on what
// is already resident), so either the engine supports demand-driven cycles or the two fuse
// into one stateful node - and that fusion is exactly this node minus the decode. The
// attractive refactor is therefore not "explode the player into plumbing" but "split player
// from codec": a media-agnostic SequencePlayer (index/window/cache/hold/retry) driving
// format-specific single-frame decode nodes (DecodeEXR, DecodeDPX, ...) through scatter/
// gather. Prerequisites either way: scatter/gather nodes, a single-frame DecodeEXR node (thin
// wrapper over ExrDecode), and pass-by-reference CPU buffer pins so ~100 MB/frame does not
// get copied through the queue.
struct ReadEXRSequenceNode : NodeContext
{
	std::mutex Mtx;
	std::condition_variable Cv;
	std::map<int, CacheEntry> Cache;   // guarded by Mtx; only the execute thread erases
	std::vector<std::string> Paths;    // snapshot of Files, guarded by Mtx
	uint64_t Gen = 0;                  // guarded by Mtx; bumped when Files changes, so workers
	                                   // still decoding the previous list discard their result

	std::atomic<int> Frame{ 0 }, Dir{ 1 }, AheadN{ 8 };
	std::atomic<bool> Stop{ false };

	int PrevFrame = 0;
	std::vector<std::thread> Workers;

	// Playback telemetry, published as engine watch logs every execution: exposes whether Index
	// outruns the decoders and whether the prefetch window is actually filling. Rates are
	// exponential moving averages of per-execution deltas.
	std::atomic<uint64_t> DecodeCount{ 0 }, DecodeUsTotal{ 0 };
	std::chrono::steady_clock::time_point LastExecTime{};
	uint64_t LastDecodes = 0, LastDecodeUs = 0;
	double EmaIndexRate = 0, EmaExecRate = 0, EmaDecodeRate = 0, EmaDecodeMs = 0;

	ReadEXRSequenceNode(nosFbNodePtr node) : NodeContext(node)
	{
		unsigned n = std::clamp(std::thread::hardware_concurrency() / 2, 4u, 16u);
		for (unsigned i = 0; i < n; ++i)
			Workers.emplace_back([this] { WorkerLoop(); });
	}

	~ReadEXRSequenceNode() override
	{
		Stop = true;
		Cv.notify_all();
		for (auto& w : Workers)
			w.join();
	}

	nosResult ExecuteNode(nosNodeExecuteParams* ep) override
	{
		nos::NodeExecuteParams params(ep);
		const FileVector* files = params.GetPinData<FileVector>(NOS_NAME_STATIC("Files"));
		const int count = files ? int(files->size()) : 0;
		if (count == 0)
		{
			SetNodeStatusMessage("Set Files list", fb::NodeStatusMessageType::WARNING);
			return NOS_RESULT_SUCCESS;
		}

		// Index wraps modulo the file count so a free-running counter loops the sequence.
		auto* indexPin = params.GetPinData<uint64_t>(NOS_NAME_STATIC("Index"));
		auto* aheadPin = params.GetPinData<uint64_t>(NOS_NAME_STATIC("PrefetchAhead"));
		int frame = int((indexPin ? *indexPin : 0) % uint64_t(count));
		int ahead = aheadPin ? int(std::clamp<uint64_t>(*aheadPin, 1, MAX_AHEAD)) : 32;
		// Signed ring distance from the previous playhead: +1 across the loop seam stays +1, so
		// direction detection and the frame-rate telemetry survive wraps.
		int prev = PrevFrame < count ? PrevFrame : 0;
		int step = ((frame - prev) % count + count + count / 2) % count - count / 2;
		int dir = step > 0 ? 1 : (step < 0 ? -1 : Dir.load());
		PrevFrame = frame;

		CacheEntry* entry = nullptr;
		std::string frameError;
		int readyAhead = 0;
		size_t cacheSize = 0;
		{
			std::lock_guard<std::mutex> lk(Mtx);
			// Any change to the file list stops the prefetch and restarts it on the new list:
			// the cache is dropped and the generation bump makes in-flight decodes discard
			// their (old-list) results when they complete.
			bool changed = Paths.size() != size_t(count);
			for (int i = 0; !changed && i < count; ++i)
				changed = Paths[i] != files->Get(i)->c_str();
			if (changed)
			{
				Paths.resize(count);
				for (int i = 0; i < count; ++i)
					Paths[i] = files->Get(i)->str();
				Cache.clear();
				++Gen;
			}
			Frame = frame; AheadN = ahead; Dir = dir;
			Evict(frame, ahead, dir);

			auto it = Cache.find(frame);
			if (it != Cache.end())
			{
				if (it->second.St == CacheEntry::State::Ready)
					entry = &it->second; // map nodes are stable; only this thread erases
				else if (it->second.St == CacheEntry::State::Failed)
					frameError = it->second.Error;
			}

			for (int k = 1; k <= ahead && k < count; ++k)
			{
				auto ahd = Cache.find(((frame + dir * k) % count + count) % count);
				if (ahd != Cache.end() && ahd->second.St == CacheEntry::State::Ready)
					++readyAhead;
			}
			cacheSize = Cache.size();
		}
		Cv.notify_all();

		// Upload and publish outside the lock: the GPU-event wait must not stall the workers.
		if (entry)
		{
			if (!entry->Uploaded)
				Upload(*entry);
			if (entry->Color)
				Publish(*entry);
			else
				frameError = "GPU upload failed";
		}

		const std::string position = std::to_string(frame) + "/" + std::to_string(count);
		if (entry && entry->Color)
			SetNodeStatusMessage("Frame " + position + " - " + std::to_string(readyAhead) + "/"
				+ std::to_string(ahead) + " ahead", fb::NodeStatusMessageType::INFO);
		else if (!frameError.empty())
			SetNodeStatusMessage("Frame " + position + ": " + frameError + " - retrying",
				fb::NodeStatusMessageType::FAILURE);
		else
		{
			char rate[32];
			snprintf(rate, sizeof(rate), "%.1f", EmaDecodeRate);
			SetNodeStatusMessage("Buffering " + position + " - " + std::to_string(readyAhead) + "/"
				+ std::to_string(ahead) + " ahead, decoding " + rate + " fps",
				fb::NodeStatusMessageType::WARNING);
		}

		LogStats(step, ahead, readyAhead, cacheSize);
		return NOS_RESULT_SUCCESS;
	}

	// Uploads a decoded frame to the GPU and frees its CPU pixels (back to the buffer pool).
	// Execute thread only; the entry is Ready, so no worker touches it.
	void Upload(CacheEntry& e)
	{
		e.Color = UploadColor(e.Frame, NodeId);
		if (e.Frame.HasDepth) e.Depth = UploadDepth(e.Frame, NodeId);
		e.Metadata = BuildExrMetadata(e.Frame);
		e.HasDepth = e.Frame.HasDepth && e.Depth.has_value();
		e.Uploaded = true;
		if (!e.Color)
		{
			e.St = CacheEntry::State::Failed;
			e.FailedAt = std::chrono::steady_clock::now();
		}
		e.Frame.Color = {};
		e.Frame.Depth = {};
	}

	// Hands a decoded + uploaded frame to the output pins. Execute thread only.
	void Publish(CacheEntry& e)
	{
		SetPinValue(NOS_NAME_STATIC("Texture"), e.Color->ToPinData());
		if (e.HasDepth && e.Depth)
			SetPinValue(NOS_NAME_STATIC("Depth"), e.Depth->ToPinData());
		if (e.Metadata)
			SetPinValue(NOS_NAME_STATIC("Metadata"), *e.Metadata);
	}

	// One worker: claim the nearest un-cached frame in the window, decode it (outside the lock),
	// store it, repeat; sleep when the window is full. A result from a previous file-list
	// generation is discarded, so a slot claimed for the old list can never publish old pixels
	// into the new sequence.
	void WorkerLoop()
	{
		std::unique_lock<std::mutex> lk(Mtx);
		while (!Stop)
		{
			int todo = Claim();
			if (todo < 0) { Cv.wait(lk); continue; }
			std::string path = Paths[todo];
			uint64_t gen = Gen;
			lk.unlock();

			ExrFrame f;
			std::string err;
			auto t0 = std::chrono::steady_clock::now();
			bool ok = false;
			try
			{
				ok = DecodeExr(path, {}, f, err);
			}
			catch (const std::exception& e)
			{
				err = e.what();
			}
			DecodeUsTotal += uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(
				std::chrono::steady_clock::now() - t0).count());
			++DecodeCount;
			if (!ok) nosEngine.LogE("ReadEXRSequence: frame %d: %s", todo, err.c_str());

			lk.lock();
			auto it = Cache.find(todo);
			if (gen == Gen && it != Cache.end() && it->second.St == CacheEntry::State::Decoding)
			{
				it->second.St = ok ? CacheEntry::State::Ready : CacheEntry::State::Failed;
				if (ok)
					it->second.Frame = std::move(f);
				else
				{
					it->second.Error = err;
					it->second.FailedAt = std::chrono::steady_clock::now();
				}
			}
			Cv.notify_all();
		}
	}

	// Nearest frame in the wrap-around window [Frame .. Frame + AheadN*Dir] (mod count) not in
	// the cache, marked Decoding. Wrapping keeps decoding the head of the sequence while its
	// tail plays, so a looping counter never hits an undecoded seam. Failed frames become
	// claimable again after FAILED_RETRY_DELAY. Holds Mtx.
	int Claim()
	{
		int frame = Frame, ahead = AheadN, dir = Dir, count = int(Paths.size());
		if (count <= 0)
			return -1;
		auto now = std::chrono::steady_clock::now();
		for (int k = 0; k <= ahead && k < count; ++k)
		{
			int f = ((frame + dir * k) % count + count) % count;
			auto it = Cache.find(f);
			if (it == Cache.end())
			{
				Cache[f].St = CacheEntry::State::Decoding;
				return f;
			}
			if (it->second.St == CacheEntry::State::Failed && now - it->second.FailedAt >= FAILED_RETRY_DELAY)
			{
				it->second = CacheEntry{}; // back to Decoding, drops any stale error/resources
				return f;
			}
		}
		return -1;
	}

	// Drop frames outside the keep window, measured as ring distance in the play direction
	// (so the window wraps with the loop); leave Decoding ones (a worker owns them). Holds Mtx.
	void Evict(int frame, int ahead, int dir)
	{
		int count = int(Paths.size());
		if (count <= 0)
			return;
		for (auto it = Cache.begin(); it != Cache.end();)
		{
			int fwd = ((it->first - frame) * dir % count + count) % count;
			bool keep = fwd <= ahead || fwd >= count - KEEP_BEHIND;
			if (!keep && it->second.St != CacheEntry::State::Decoding)
				it = Cache.erase(it);
			else
				++it;
		}
	}

	// Publishes playback telemetry as engine watch logs, every execution. Rates are EMAs of
	// per-execution deltas: playhead vs wall clock (does Index outrun real time?), execution
	// rate, decode throughput and latency, prefetch fill.
	void LogStats(int step, int ahead, int readyAhead, size_t cacheSize)
	{
		nosEngine.WatchLog("ReadEXRSequence ready-ahead",
			(std::to_string(readyAhead) + " / " + std::to_string(ahead)).c_str());
		nosEngine.WatchLog("ReadEXRSequence cached frames", std::to_string(cacheSize).c_str());

		auto now = std::chrono::steady_clock::now();
		uint64_t decodes = DecodeCount.load(), decodeUs = DecodeUsTotal.load();
		if (LastExecTime.time_since_epoch().count() != 0)
		{
			double dt = std::chrono::duration<double>(now - LastExecTime).count();
			if (dt > 0.0 && dt < 1.0) // skip pauses so a stall doesn't poison the averages
			{
				constexpr double ALPHA = 0.05;
				EmaIndexRate += ALPHA * (step / dt - EmaIndexRate);
				EmaExecRate += ALPHA * (1.0 / dt - EmaExecRate);
				EmaDecodeRate += ALPHA * (double(decodes - LastDecodes) / dt - EmaDecodeRate);
				if (decodes > LastDecodes)
					EmaDecodeMs += 0.2 * (double(decodeUs - LastDecodeUs) / 1000.0
						/ double(decodes - LastDecodes) - EmaDecodeMs);
			}
			char buf[64];
			snprintf(buf, sizeof(buf), "%.1f", EmaIndexRate);
			nosEngine.WatchLog("ReadEXRSequence index rate (frames/s)", buf);
			snprintf(buf, sizeof(buf), "%.1f", EmaExecRate);
			nosEngine.WatchLog("ReadEXRSequence exec rate (/s)", buf);
			snprintf(buf, sizeof(buf), "%.1f", EmaDecodeRate);
			nosEngine.WatchLog("ReadEXRSequence decode rate (frames/s)", buf);
			snprintf(buf, sizeof(buf), "%.0f", EmaDecodeMs);
			nosEngine.WatchLog("ReadEXRSequence decode avg (ms)", buf);
		}
		LastExecTime = now;
		LastDecodes = decodes;
		LastDecodeUs = decodeUs;
	}
};

nosResult RegisterReadEXRSequence(nosNodeFunctions* fn)
{
	NOS_BIND_NODE_CLASS(NOS_NAME_STATIC("ReadEXRSequence"), ReadEXRSequenceNode, fn);
	return NOS_RESULT_SUCCESS;
}

} // namespace nos::mediaio
