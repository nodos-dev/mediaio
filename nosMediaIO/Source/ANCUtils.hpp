// Copyright MediaZ Teknoloji A.S. All Rights Reserved.
#pragma once

#include "ANC_generated.h"

namespace nos::mediaio
{

// Rebuild one incoming ANC packet into `fbb` verbatim — every field copied,
// including `coding`. Shared by the Inject* nodes that forward pass-through
// packets alongside the one they insert, so a forwarded packet is byte-for-byte
// what arrived.
inline flatbuffers::Offset<ANCPacket> CloneANCPacket(flatbuffers::FlatBufferBuilder& fbb, const ANCPacket* src)
{
	const auto* p = src->payload();
	auto payloadOffset = fbb.CreateVector(p ? p->data() : nullptr, p ? p->size() : 0);
	ANCPacketBuilder pb(fbb);
	pb.add_did(src->did());
	pb.add_sdid(src->sdid());
	pb.add_line_number(src->line_number());
	pb.add_horiz_offset(src->horiz_offset());
	pb.add_space(src->space());
	pb.add_channel(src->channel());
	pb.add_link(src->link());
	pb.add_stream(src->stream());
	pb.add_is_field2(src->is_field2());
	pb.add_coding(src->coding());
	pb.add_payload(payloadOffset);
	return pb.Finish();
}

} // namespace nos::mediaio
