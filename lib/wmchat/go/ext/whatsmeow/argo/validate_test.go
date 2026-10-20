package argo

import (
	"encoding/binary"
	"testing"

	"github.com/beeper/argo-go/codec"
	"github.com/beeper/argo-go/header"
	"github.com/beeper/argo-go/pkg/buf"
	"github.com/beeper/argo-go/wire"
	"github.com/elliotchance/orderedmap/v3"
)

func typedTestFrame(core []byte) []byte {
	return typedTestSegments(core)
}

func typedTestSegments(segments ...[]byte) []byte {
	frame := []byte{0}
	for _, segment := range segments {
		frame = binary.AppendVarint(frame, int64(len(segment)))
		frame = append(frame, segment...)
	}
	return frame
}

func TestValidateTypedFrameRoundTrip(t *testing.T) {
	stringBlock := wire.NewBlockType(wire.String, "String", true)
	schema := wire.RecordType{Fields: []wire.Field{
		{Name: "names", Of: wire.ArrayType{Of: wire.NullableType{Of: stringBlock}}},
		{Name: "count", Of: wire.NewBlockType(wire.Varint, "Int", false)},
		{Name: "extra", Of: wire.Desc, Omittable: true},
	}}
	input := orderedmap.NewOrderedMap[string, any]()
	input.Set("names", []any{"alpha", nil, "alpha", "beta"})
	input.Set("count", int64(42))
	for _, flags := range []byte{0, 4, 32, 64} {
		encoder := codec.NewArgoEncoder()
		encoder.Header().SetFlag(header.HeaderSelfDescribingFlag, flags&4 != 0)
		encoder.Header().SetFlag(header.HeaderNullTerminatedStringsFlag, flags&32 != 0)
		encoder.Header().SetFlag(header.HeaderNoDeduplicationFlag, flags&64 != 0)
		if err := encoder.ValueToArgoWithType(input, schema); err != nil {
			t.Fatal(err)
		}
		frame, err := encoder.GetResult()
		if err != nil {
			t.Fatal(err)
		}
		if err := ValidateTypedFrame(frame.Bytes(), schema); err != nil {
			t.Fatalf("header %x: %v", flags, err)
		}
		decoder, err := codec.NewArgoDecoder(buf.NewBufReadonly(frame.Bytes()))
		if err != nil {
			t.Fatal(err)
		}
		decoded, err := decoder.ArgoToMap(schema)
		if err != nil {
			t.Fatal(err)
		}
		if count, ok := decoded.Get("count"); !ok || count != int64(42) {
			t.Fatalf("count did not round-trip: %v", count)
		}
	}
}

func TestValidateTypedFrameScalarBlocks(t *testing.T) {
	stringSchema := wire.RecordType{Fields: []wire.Field{{Name: "string", Of: wire.NewBlockType(wire.String, "String", true)}}}
	intSchema := wire.RecordType{Fields: []wire.Field{{Name: "integer", Of: wire.NewBlockType(wire.Varint, "Int", false)}}}
	floatSchema := wire.RecordType{Fields: []wire.Field{{Name: "float", Of: wire.NewBlockType(wire.Float64, "Float", false)}}}
	for _, test := range []struct {
		name   string
		frame  []byte
		schema wire.Type
	}{
		{"numeric overflow", typedTestSegments([]byte{128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 0}, nil), intSchema},
		{"numeric truncation", typedTestSegments([]byte{128}, nil), intSchema},
		{"float truncation", typedTestSegments([]byte{0}, nil), floatSchema},
		{"string truncation", typedTestSegments([]byte{'x'}, []byte{4}), stringSchema},
		{"missing scalar block", typedTestFrame([]byte{0}), stringSchema},
		{"bad reference", typedTestSegments(nil, binary.AppendVarint(nil, -4)), stringSchema},
		{"extra scalar block", typedTestSegments([]byte{0}, []byte{0}, nil), intSchema},
		{"unread scalar bytes", typedTestSegments([]byte{'x', 'y'}, []byte{2}), stringSchema},
	} {
		t.Run(test.name, func(t *testing.T) {
			if err := ValidateTypedFrame(test.frame, test.schema); err == nil {
				t.Fatal("malformed scalar block accepted")
			}
		})
	}
	conflicting := wire.RecordType{Fields: []wire.Field{
		{Name: "first", Of: wire.NewBlockType(wire.String, "same", true)},
		{Name: "second", Of: wire.NewBlockType(wire.Varint, "same", false)},
	}}
	if err := ValidateTypedFrame(typedTestSegments(nil, []byte{0}), conflicting); err == nil {
		t.Fatal("conflicting block definitions accepted")
	}
}

func TestValidateTypedFrameBoundsJSONReferenceExpansion(t *testing.T) {
	core := binary.AppendVarint(nil, 1000)
	core = binary.AppendVarint(core, 64<<10)
	for i := 1; i < 1000; i++ {
		core = binary.AppendVarint(core, -4)
	}
	schema := wire.RecordType{Fields: []wire.Field{{Name: "strings", Of: wire.ArrayType{Of: wire.NewBlockType(wire.String, "String", true)}}}}
	// Only ~66 KiB on the wire, but JSON would expand this to at least 64 MiB.
	if err := ValidateTypedFrame(typedTestSegments(make([]byte, 64<<10), core), schema); err == nil {
		t.Fatal("deduplicated JSON amplification accepted")
	}
}

func TestValidateTypedFrameRejectsUnsafeLengths(t *testing.T) {
	stringBlock := wire.NewBlockType(wire.String, "String", true)
	arraySchema := wire.RecordType{Fields: []wire.Field{{Name: "array", Of: wire.ArrayType{Of: wire.Boolean}}}}
	stringSchema := wire.RecordType{Fields: []wire.Field{{Name: "string", Of: stringBlock}}}
	for _, test := range []struct {
		name   string
		frame  []byte
		schema wire.Type
	}{
		{"huge array", typedTestFrame(binary.AppendVarint(nil, 1<<60)), arraySchema},
		{"huge string", typedTestFrame(binary.AppendVarint(nil, 1<<60)), stringSchema},
		{"negative array", typedTestFrame(binary.AppendVarint(nil, -1)), arraySchema},
		{"truncated array", typedTestFrame([]byte{2}), arraySchema},
		{"overflow label", typedTestFrame([]byte{128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 0}), arraySchema},
		{"huge segment", append([]byte{0}, binary.AppendVarint(nil, 1<<60)...), arraySchema},
		{"truncated segment", []byte{0, 4, 0}, arraySchema},
		{"trailing core", typedTestFrame([]byte{0, 0}), arraySchema},
		{"inline", []byte{2, 0}, wire.RecordType{}},
		{"user flags", []byte{128, 0}, wire.RecordType{}},
		{"extended header", []byte{1, 0, 0}, wire.RecordType{}},
		{"too large frame", make([]byte, maxTypedFrameBytes+1), wire.RecordType{}},
		{"nil schema", typedTestFrame(nil), nil},
	} {
		t.Run(test.name, func(t *testing.T) {
			if err := ValidateTypedFrame(test.frame, test.schema); err == nil {
				t.Fatal("unsafe frame accepted")
			}
		})
	}
}

func TestValidateTypedFrameRejectsUnboundedExtensions(t *testing.T) {
	for _, kind := range []wire.Type{wire.Desc, wire.Extensions, wire.Path} {
		schema := wire.RecordType{Fields: []wire.Field{{Name: "extension", Of: kind, Omittable: true}}}
		if err := ValidateTypedFrame(typedTestFrame([]byte{3}), schema); err != nil {
			t.Fatalf("absent %T rejected: %v", kind, err)
		}
		if err := ValidateTypedFrame(typedTestFrame([]byte{0, 0}), schema); err == nil {
			t.Fatalf("present %T accepted", kind)
		}
	}
	schema := wire.RecordType{Fields: []wire.Field{{Name: "data", Of: wire.NullableType{Of: wire.RecordType{}}}}}
	if err := ValidateTypedFrame(typedTestFrame([]byte{5, 0}), schema); err == nil {
		t.Fatal("inline field error accepted")
	}
}

func TestValidateTypedFrameLimitsDepthAndAllocations(t *testing.T) {
	var nested wire.Type = wire.RecordType{}
	for i := 0; i < maxTypedDepth+1; i++ {
		nested = wire.RecordType{Fields: []wire.Field{{Name: "nested", Of: nested}}}
	}
	if err := ValidateTypedFrame(typedTestFrame(nil), nested); err == nil {
		t.Fatal("deep schema accepted")
	}
	// Empty record array elements need no core bytes: the validator must still
	// charge their fixed schema expansion, not assume input size bounds output.
	fields := make([]wire.Field, 32)
	for i := range fields {
		fields[i] = wire.Field{Name: "empty", Of: wire.RecordType{}}
	}
	schema := wire.RecordType{Fields: []wire.Field{{Name: "records", Of: wire.ArrayType{Of: wire.RecordType{Fields: fields}}}}}
	if err := ValidateTypedFrame(typedTestFrame(binary.AppendVarint(nil, maxTypedArray)), schema); err == nil {
		t.Fatal("allocation-heavy schema accepted")
	}
	segments := []byte{0}
	for i := 0; i < 257; i++ {
		segments = append(segments, 0)
	}
	if err := ValidateTypedFrame(segments, wire.RecordType{}); err == nil {
		t.Fatal("excess segments accepted")
	}
}
