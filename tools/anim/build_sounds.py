"""Converts JC2's grapple and parachute sounds for the plugin (JC2 data: written to Skyrim's Data
folder and extracted/sound, never to git).

Source: Parachute.fsb (top-level archive entry, FMOD FSB4), the wave bank of parachute.fev's
events (Parachute_Horiz / _Vert / _InFlight / _steer, grappling_fire_wire, grappling_reel_loop,
grappling_reel_back, grappling_impact_*, grappling_detatch). Every sample is MPEG (mode 0x200,
0x40 stereo / 0x20 mono) layer III, its frames padded apart; the frames are packed back to back
and decoded with miniaudio (pip install miniaudio; within 1 LSB of ffmpeg) to 16-bit PCM WAV,
the format Skyrim plays from loose files. Loops are written repeated LOOP_SECONDS long, so the
plugin's restart is rare.

FSB4: "FSB4", u32 samples, u32 sample header bytes, u32 data bytes, u32 version, u32 mode, 24
bytes, then per sample u16 header size, name[30], u32 length (samples), u32 compressed bytes,
u32 loop start, u32 loop end, u32 mode, i32 frequency, ...; sample data back to back.

python build_sounds.py [--data <Skyrim Data dir>]"""
import argparse, os, shutil, struct, sys, wave

import miniaudio

import paths
from bsa import SKYRIM_DATA

sys.path.insert(0, os.path.join(paths.REPO, "tools", "re"))
import jc2arc  # noqa: E402

OUT = os.path.join("sound", "fx", "jc2mech")
LOOP_SECONDS = 30.0
# Samples the plugin plays (skse/src/sounds.cpp): output name -> (JC2 sample, loop)
SOUNDS = {
    "grapple_fire": ("grappling_fire_02.wav", False),
    "grapple_wire_1": ("grappling_fire_wire_01.wav", False),
    "grapple_wire_2": ("grappling_fire_wire_02.wav", False),
    "grapple_wire_3": ("grappling_fire_wire_03.wav", False),
    "grapple_impact_1": ("grappling_impact_04.wav", False),
    "grapple_impact_2": ("grappling_impact_06.wav", False),
    "grapple_impact_rock_1": ("grappling_impact_rock_01.wav", False),
    "grapple_impact_rock_2": ("grappling_impact_rock_02.wav", False),
    "grapple_impact_metal_1": ("grappling_impact_metal_01.wav", False),
    "grapple_twang_1": ("grappling_wiretwang_01.wav", False),
    "grapple_twang_2": ("grappling_wiretwang_02.wav", False),
    "grapple_reel": ("Reel_loop_01.wav", True),
    "grapple_wire_tone": ("wire_tone_loop_02.wav", True),
    "grapple_reelback_1": ("grappling_reelback_03.wav", False),
    "grapple_reelback_2": ("grappling_reelback_04.wav", False),
    "grapple_reelback_3": ("grappling_reelback_05.wav", False),
    "grapple_detach": ("grappling_detatch_01.wav", False),
    "chute_open": ("Para_Flap_Open_05.wav", False),
    "chute_flap_slow": ("Para_FlappingSlow_exp 001.wav", True),
    "chute_flap_fast": ("Para_FlappingFast_L.wav", True),
    "chute_steer_1": ("Parachute-steer-01.wav", False),
    "chute_steer_2": ("Parachute-steer-02.wav", False),
    "chute_steer_3": ("Parachute-steer-03.wav", False),
    "chute_steer_4": ("Parachute-steer-04.wav", False),
    "chute_wind": ("Steady wind 7.wav", True),
    "freefall_wind": ("Wind Freefall_35.wav", True),
}


def read_fsb(data):
    magic, n, shsize, _, _, _ = struct.unpack_from("<4s5I", data, 0)
    if magic != b"FSB4":
        raise ValueError("not an FSB4 bank")
    out = {}
    o, d = 48, 48 + shsize
    for _ in range(n):
        size, = struct.unpack_from("<H", data, o)
        name = data[o + 2:o + 32].split(b"\0")[0].decode("latin1")
        length, comp, _, _, mode, freq = struct.unpack_from("<5Ii", data, o + 32)
        channels, = struct.unpack_from("<H", data, o + 62)
        out[name] = dict(data=data[d:d + comp], mode=mode, freq=freq, channels=channels, length=length)
        o += size
        d += comp
    return out


# MPEG audio frame header: kbps by bitrate index (MPEG-1, MPEG-2/2.5 layer III), Hz by version.
BITRATES = {1: [0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320],
            2: [0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160]}
RATES = {3: [44100, 48000, 32000], 2: [22050, 24000, 16000], 0: [11025, 12000, 8000]}


def mp3_frames(data):
    """The layer III frames of an FSB sample, back to back (FMOD pads them apart, which
    miniaudio's decoder doesn't skip)."""
    out, o = bytearray(), 0
    while o + 4 <= len(data):
        h = int.from_bytes(data[o:o + 4], "big")
        ver, layer, br, sr, pad = (h >> 19) & 3, (h >> 17) & 3, (h >> 12) & 15, (h >> 10) & 3, (h >> 9) & 1
        if h >> 21 != 0x7FF or ver == 1 or layer != 1 or br in (0, 15) or sr == 3:
            o += 1
            continue
        size = (144 if ver == 3 else 72) * BITRATES[1 if ver == 3 else 2][br] * 1000 // RATES[ver][sr] + pad
        out += data[o:o + size]
        o += size
    return bytes(out)


def bank():
    want = jc2arc.name_hash("Parachute.fsb")
    for _, h, off, size, f in jc2arc.files():
        if h == want:
            return jc2arc.data(f, off, size)
    raise FileNotFoundError("Parachute.fsb not in the JC2 archives")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--data", default=SKYRIM_DATA)
    args = ap.parse_args()
    samples = read_fsb(bank())
    stage = os.path.join(paths.EXTRACTED, "sound", "jc2mech")
    os.makedirs(stage, exist_ok=True)
    for out, (src, loop) in SOUNDS.items():
        s = samples[src]
        if not s["mode"] & 0x200:
            raise ValueError("%s: mode %08x is not MPEG" % (src, s["mode"]))
        pcm = miniaudio.mp3_read_s16(mp3_frames(s["data"]))
        if pcm.num_frames != s["length"] or pcm.nchannels != s["channels"] or pcm.sample_rate != s["freq"]:
            raise ValueError("%s: decoded %d frames x %d ch at %d Hz, the bank says %d x %d at %d" % (
                src, pcm.num_frames, pcm.nchannels, pcm.sample_rate, s["length"], s["channels"], s["freq"]))
        frames = pcm.samples.tobytes()
        if loop:
            frames *= max(1, int(LOOP_SECONDS / max(s["length"] / s["freq"], 0.1)))
        wav = os.path.join(stage, out + ".wav")
        with wave.open(wav, "wb") as w:
            w.setnchannels(s["channels"])
            w.setsampwidth(2)
            w.setframerate(s["freq"])
            w.writeframes(frames)
        print("%-24s %-32s %s %5.2f s" % (out, src, "loop" if loop else "    ", len(frames) / (2 * s["channels"] * s["freq"])))
    dest = os.path.join(args.data, OUT)
    os.makedirs(dest, exist_ok=True)
    for f in os.listdir(stage):
        shutil.copyfile(os.path.join(stage, f), os.path.join(dest, f))
    print("installed to", dest)


if __name__ == "__main__":
    main()
