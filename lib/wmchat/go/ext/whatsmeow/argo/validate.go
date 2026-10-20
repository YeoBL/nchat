package argo

import (
	"encoding/binary"
	"fmt"

	"github.com/beeper/argo-go/wire"
)

const (
	maxTypedFrameBytes = 2 << 20
	maxTypedDepth      = 64
	maxTypedNodes      = 100000
	maxTypedAllocation = 8 << 20
	maxTypedArray      = 10000
)

// ValidateTypedFrame checks allocation-bearing labels before argo-go's decoder,
// which otherwise allocates from untrusted lengths before checking their data.
// This deliberately supports only bounded, non-inline typed frames. Optional
// self-describing extensions may be absent, but present ones fail closed.
func ValidateTypedFrame(data []byte, schema wire.Type) error {
	if len(data) < 2 || len(data) > maxTypedFrameBytes {
		return fmt.Errorf("unsupported Argo frame size: %d", len(data))
	}
	// Argo's header uses the low bit for continuation; its other seven bits
	// contain all known flags. Reject extended/user headers and inline values.
	if data[0]&(1|2|128) != 0 {
		return fmt.Errorf("unsupported Argo header flags: 0x%02x", data[0])
	}
	var segments [][]byte
	for offset := 1; offset < len(data); {
		if len(segments) >= 256 {
			return fmt.Errorf("too many Argo segments")
		}
		size, n := binary.Varint(data[offset:])
		if n <= 0 || size < 0 || size > int64(len(data)-offset-n) {
			return fmt.Errorf("invalid Argo segment length")
		}
		offset += n
		segments = append(segments, data[offset:offset+int(size)])
		offset += int(size)
	}
	core := segments[len(segments)-1]
	v := typedFrameValidator{
		core: core, budget: maxTypedAllocation, segments: segments[:len(segments)-1],
		blocks: make(map[wire.BlockKey]*typedScalarBlock), nullTerminated: data[0]&32 != 0,
	}
	if err := v.value(schema, nil, 0); err != nil {
		return fmt.Errorf("unsafe or unsupported typed Argo frame: %w", err)
	}
	if v.offset != len(core) {
		return fmt.Errorf("trailing typed Argo core data")
	}
	if v.nextBlock != len(v.segments) {
		return fmt.Errorf("unexpected typed Argo scalar blocks")
	}
	for _, block := range v.blocks {
		if block.offset != len(block.data) {
			return fmt.Errorf("trailing typed Argo scalar data")
		}
	}
	return nil
}

type typedFrameValidator struct {
	core           []byte
	offset         int
	nodes          int
	budget         int64
	segments       [][]byte
	nextBlock      int
	blocks         map[wire.BlockKey]*typedScalarBlock
	nullTerminated bool
}

type typedScalarBlock struct {
	definition wire.BlockType
	kind       wire.Type
	data       []byte
	offset     int
	references []int64
}

func (v *typedFrameValidator) scalarBlock(definition *wire.BlockType, kind wire.Type) (*typedScalarBlock, error) {
	if definition == nil {
		return nil, fmt.Errorf("Argo scalar has no block")
	}
	if existing := v.blocks[definition.Key]; existing != nil {
		if existing.kind != kind || existing.definition.Dedupe != definition.Dedupe {
			return nil, fmt.Errorf("inconsistent Argo scalar block definition")
		}
		return existing, nil
	}
	if v.nextBlock >= len(v.segments) {
		return nil, fmt.Errorf("missing typed Argo scalar block")
	}
	block := &typedScalarBlock{definition: *definition, kind: kind, data: v.segments[v.nextBlock]}
	v.blocks[definition.Key] = block
	v.nextBlock++
	return block, nil
}

func (b *typedScalarBlock) advance(size int64) error {
	if size < 0 || size > int64(len(b.data)-b.offset) {
		return fmt.Errorf("truncated typed Argo scalar block")
	}
	b.offset += int(size)
	return nil
}

func (v *typedFrameValidator) charge(size int64) error {
	if size < 0 || size > v.budget {
		return fmt.Errorf("Argo allocation budget exceeded")
	}
	v.budget -= size
	return nil
}

func (v *typedFrameValidator) peek() (byte, error) {
	if v.offset >= len(v.core) {
		return 0, fmt.Errorf("truncated Argo core")
	}
	return v.core[v.offset], nil
}

func (v *typedFrameValidator) label() (int64, error) {
	if v.offset >= len(v.core) {
		return 0, fmt.Errorf("truncated Argo label")
	}
	x, n := binary.Varint(v.core[v.offset:])
	if n <= 0 {
		return 0, fmt.Errorf("invalid Argo label")
	}
	v.offset += n
	return x, nil
}

func typedIsLabeled(t wire.Type) (bool, error) {
	for i := 0; i < maxTypedDepth && t != nil; i++ {
		switch value := t.(type) {
		case wire.BlockType:
			t = value.Of
		case wire.NullableType, wire.StringType, wire.BytesType, wire.BooleanType, wire.ArrayType:
			return true, nil
		default:
			return false, nil
		}
	}
	return false, fmt.Errorf("invalid or deeply nested Argo schema")
}

func (v *typedFrameValidator) value(t wire.Type, block *wire.BlockType, depth int) error {
	if t == nil || depth > maxTypedDepth || v.nodes >= maxTypedNodes {
		return fmt.Errorf("Argo schema depth or node limit exceeded")
	}
	v.nodes++
	// Include path copies and decoder bookkeeping, not just output values.
	if err := v.charge(256 + int64(depth)*16); err != nil {
		return err
	}
	switch value := t.(type) {
	case wire.BlockType:
		return v.value(value.Of, &value, depth+1)
	case wire.NullableType:
		marker, err := v.peek()
		if err != nil {
			return err
		}
		if marker == 1 || marker == 3 { // Canonical null / absent labels.
			v.offset++
			return nil
		}
		if marker == 5 {
			return fmt.Errorf("inline Argo field errors are unsupported")
		}
		labeled, err := typedIsLabeled(value.Of)
		if err != nil {
			return err
		}
		if !labeled {
			x, err := v.label()
			if err != nil || x != 0 {
				return fmt.Errorf("invalid Argo non-null marker")
			}
		}
		return v.value(value.Of, block, depth+1)
	case wire.RecordType:
		if err := v.charge(int64(len(value.Fields)) * 256); err != nil {
			return err
		}
		for _, field := range value.Fields {
			if field.Omittable {
				marker, err := v.peek()
				if err != nil {
					return err
				}
				labeled, err := typedIsLabeled(field.Of)
				if err != nil {
					return err
				}
				if marker == 3 {
					v.offset++
					continue
				}
				if !labeled && marker == 0 {
					v.offset++
				}
			}
			if err := v.value(field.Of, block, depth+1); err != nil {
				return err
			}
		}
		return nil
	case wire.ArrayType:
		size, err := v.label()
		if err != nil || size < 0 || size > maxTypedArray {
			return fmt.Errorf("invalid or oversized Argo array")
		}
		if err := v.charge(size * 16); err != nil {
			return err
		}
		for i := int64(0); i < size; i++ {
			if err := v.value(value.Of, block, depth+1); err != nil {
				return err
			}
		}
		return nil
	case wire.BooleanType:
		x, err := v.label()
		if err != nil || (x != 0 && x != 1) {
			return fmt.Errorf("invalid Argo boolean")
		}
		return nil
	case wire.StringType, wire.BytesType:
		data, err := v.scalarBlock(block, t)
		if err != nil {
			return err
		}
		size, err := v.label()
		if err != nil {
			return err
		}
		if size < -3 && block.Dedupe {
			index := -(size + 4)
			if index >= int64(len(data.references)) {
				return fmt.Errorf("invalid typed Argo backreference")
			}
			// JSON serialization expands aliases again, including up to six
			// output bytes for each escaped string byte. Bound that output too.
			return v.charge(data.references[index]*6 + 32)
		}
		if size < 0 || size > maxTypedFrameBytes {
			return fmt.Errorf("invalid or oversized Argo scalar")
		}
		if err := data.advance(size); err != nil {
			return err
		}
		if _, isString := t.(wire.StringType); isString && v.nullTerminated {
			if err := data.advance(1); err != nil {
				return err
			}
			if data.data[data.offset-1] != 0 {
				return fmt.Errorf("invalid Argo string terminator")
			}
		}
		if block.Dedupe {
			data.references = append(data.references, size)
		}
		return v.charge(size*8 + 32) // Decoder copies plus escaped JSON output.
	case wire.FixedType:
		if block == nil || value.Length < 0 || value.Length > maxTypedFrameBytes {
			return fmt.Errorf("invalid Argo fixed scalar")
		}
		if block.Dedupe {
			return fmt.Errorf("unsupported deduplicated Argo fixed scalar")
		}
		data, err := v.scalarBlock(block, t)
		if err != nil {
			return err
		}
		if err := data.advance(int64(value.Length)); err != nil {
			return err
		}
		return v.charge(int64(value.Length))
	case wire.VarintType, wire.Float64Type:
		data, err := v.scalarBlock(block, t)
		if err != nil {
			return err
		}
		if block.Dedupe {
			return fmt.Errorf("unsupported deduplicated Argo numeric scalar")
		}
		if _, isFloat := t.(wire.Float64Type); isFloat {
			return data.advance(8)
		}
		_, n := binary.Varint(data.data[data.offset:])
		if n <= 0 {
			return fmt.Errorf("invalid typed Argo numeric varint")
		}
		data.offset += n
		return nil
	default:
		return fmt.Errorf("unsupported Argo type %T", t)
	}
}
