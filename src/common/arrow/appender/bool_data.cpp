#include "duckdb/common/arrow/arrow_appender.hpp"
#include "duckdb/common/arrow/appender/bool_data.hpp"

namespace duckdb {

void ArrowBoolData::Initialize(ArrowAppendData &result, const LogicalType &type, idx_t capacity) {
	auto byte_count = (capacity + 7) / 8;
	result.GetMainBuffer().reserve(byte_count);
}

void ArrowBoolData::Append(ArrowAppendData &append_data, Vector &input, idx_t from, idx_t to, idx_t input_size) {
	idx_t size = to - from;
	UnifiedVectorFormat format;
	input.ToUnifiedFormat(input_size, format);
	auto &main_buffer = append_data.GetMainBuffer();
	auto &validity_buffer = append_data.GetValidityBuffer();
	// we initialize both the validity and the bit set to 1's
	ArrowAppendData::ResizeValidity(validity_buffer, append_data.row_count + size);
	ArrowAppendData::ResizeValidity(main_buffer, append_data.row_count + size);
	auto data = UnifiedVectorFormat::GetData<bool>(format);

	auto result_data = main_buffer.GetData<uint8_t>();
	auto validity_data = validity_buffer.GetData<uint8_t>();
	uint8_t current_bit;
	idx_t current_byte;
	ArrowAppendData::GetBitPosition(append_data.row_count, current_byte, current_bit);
	for (idx_t i = from; i < to; i++) {
		auto source_idx = format.sel->get_index(i);
		// append the validity mask
		if (!format.validity.RowIsValid(source_idx)) {
			append_data.SetNull(validity_data, current_byte, current_bit);
		} else if (!data[source_idx]) {
			ArrowAppendData::UnsetBit(result_data, current_byte, current_bit);
		}
		ArrowAppendData::NextBit(current_byte, current_bit);
	}
	append_data.row_count += size;
}

void ArrowBoolData::Finalize(ArrowAppendData &append_data, const LogicalType &type, ArrowArray *result) {
	result->n_buffers = 2;
	result->buffers[1] = append_data.GetMainBuffer().data();
}

} // namespace duckdb
