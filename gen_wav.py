import wave, math, struct

with wave.open("test.wav", "w") as f:
    f.setnchannels(1)
    f.setsampwidth(1)
    f.setframerate(11025)
    # Generate 1 second of 440Hz sine wave
    for i in range(11025 * 3): # 3 seconds
        v = int((math.sin(2 * math.pi * 440.0 * (i / 11025.0)) + 1.0) * 127.5)
        f.writeframesraw(struct.pack('<B', v))

print("Created test.wav")
