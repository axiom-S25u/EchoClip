about echoclip
echoclip is a background recorder for geometry dash that actually works without killing your pc. most recorders use raw frame buffers that eat like 70gb of ram, but this pipes encoded video straight to your disk instead.

how it works
it uses a sliding window to track your last 5 attempts using a deque of markers. when you hit the save hotkey, it uses ffmpeg to seek and trim exactly what you need from the temp file.

technical stuff
audio: manually mixes fmod game audio and your mic into one stream.

encoding: uses nvenc/cuda hardware acceleration so it doesn't lag your physics.

buffer: 27,000 frames providing a 15 minute rolling window.

performance: background threaded so the game stays at your monitor's refresh rate.

written by axiom because i needed a clipper that didn't crash on xl levels.