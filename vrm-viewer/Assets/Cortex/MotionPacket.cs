using System;
using System.IO;

namespace Cortex.Vrm
{
    // Shared with C++ encodeMotion. No Unity dependencies; also tested by .NET.
    public sealed class MotionPacket
    {
        public const int BoneCount = 38;
        public const int ExpressionCount = 8;
        public const int ByteCount = 32 + BoneCount * 20;
        public const int ExtendedByteCount = ByteCount + ExpressionCount * 4;
        public ulong Sequence, TimestampUs;
        public readonly float[] Values = new float[BoneCount * 5];
        public readonly float[] Expressions = new float[ExpressionCount];
        public bool HasExpressions;
        public static bool TryParse(byte[] bytes, out MotionPacket packet)
        {
            packet = null;
            if (bytes == null || (bytes.Length != ByteCount && bytes.Length != ExtendedByteCount)) return false;
            using (var stream = new MemoryStream(bytes, false))
            using (var reader = new BinaryReader(stream))
            {
                if (System.Text.Encoding.ASCII.GetString(reader.ReadBytes(8)) != "CXVRM001" ||
                    reader.ReadUInt32() != 1 || reader.ReadUInt32() != BoneCount) return false;
                var value = new MotionPacket { Sequence = reader.ReadUInt64(), TimestampUs = reader.ReadUInt64() };
                for (int i = 0; i < value.Values.Length; ++i)
                {
                    var f = reader.ReadSingle();
                    if (float.IsNaN(f) || float.IsInfinity(f)) return false;
                    value.Values[i] = f;
                }
                for (int i = 0; i < BoneCount; ++i)
                {
                    int j = i * 5;
                    float norm = 0;
                    for (int k = 0; k < 4; ++k) norm += value.Values[j + k] * value.Values[j + k];
                    if (norm < .99f || norm > 1.01f || value.Values[j + 4] < 0 || value.Values[j + 4] > 1) return false;
                }
                if (bytes.Length == ExtendedByteCount)
                {
                    value.HasExpressions = true;
                    for (int i = 0; i < ExpressionCount; ++i)
                    {
                        var f = reader.ReadSingle();
                        if (float.IsNaN(f) || float.IsInfinity(f)) return false;
                        value.Expressions[i] = f;
                    }
                }
                packet = value; return true;
            }
        }
    }
}
