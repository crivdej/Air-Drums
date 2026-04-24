import wave

def convert(wav_path, var_name, out_path):
    with wave.open(wav_path, 'rb') as w:
        assert w.getnchannels() == 1, f"{wav_path} must be mono"
        assert w.getsampwidth() == 1, f"{wav_path} must be 8-bit"
        assert w.getframerate() == 16000, f"{wav_path} must be 16kHz"
        data = w.readframes(w.getnframes())
    
    with open(out_path, 'w') as f:
        f.write(f"#pragma once\n#include <Arduino.h>\n\n")
        f.write(f"const uint32_t {var_name}_length = {len(data)};\n")
        f.write(f"const uint8_t {var_name}_data[] PROGMEM = {{\n  ")
        for i, b in enumerate(data):
            f.write(f"0x{b:02x},")
            if (i+1) % 16 == 0: f.write("\n  ")
        f.write("\n};\n")

convert("kick.wav", "kick", "kick.h")
convert("snare.wav", "snare", "snare.h")
convert("hihat.wav", "hihat", "hihat.h")
print("Done — kick.h, snare.h, hihat.h created")