# Integration test: the Sound control panel records what it is given.
#
# The AV suite's av-sound-in row already drives this same GUI and pins the
# result byte-exactly against a golden.  A golden cannot answer the question
# this test asks, because a golden pins whatever the code does today —
# including a recording that is entirely wrong.  This test compares the
# playback against the SIGNAL THAT WAS FED IN, so it fails when the guest
# records something that is not what it heard.
#
# It exists because of a specific defect.  The guest's Sound Manager record
# AGC has ~78 dB of range: fed the codec's own +-1 LSB dither it returns a
# full-scale, flat, structureless roar (measured: -2.3 dBFS quietest tenth,
# -1.8 dBFS loudest, 0.5 dB of dynamic range across five seconds).  So a
# capture path delivering NOTHING and one delivering speech are
# indistinguishable by ear and by peak level alike, and every cheap
# assertion — samples flowed, peak is high, DMA ran — passes on silence.
# Only comparing the shape of the recording against the shape of the input
# separates them.
#
# Two steps, because the comparison is signal analysis and the script
# language reads no WAVs: the emulator dumps the played-back capture, then
# compare.py measures it against the input.

TEST_NAME := AV Sound cdev record round-trip
TEST_DESC := Record a known utterance through the Sound control panel and compare the playback against the input

TEST_ROM := roms/q840av-q660av-5bf10fd1.rom

TEST_RUNNER := run.sh

# Extended: a full System 7.1 boot plus a GUI drive through two nested
# dialogs, then playback in real time.
TEST_TIER := extended
