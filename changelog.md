v1.0.0 - initial release
technical changes
direct-to-disk encoding: scrapped the ram-heavy buffer to prevent memory leaks. writes straight to the filesystem now.

sliding window buffer: added a deque of AttemptMarker objects to track exactly where the last 5 attempts start and end.

fmod audio mixing: manually mixed game audio and microphone streams at a 70/30 ratio.

hardware acceleration: added NVENC/CUDA support to keep game physics stable while recording.

automatic trimming: implemented an ffmpeg seek-and-trim system for the save hotkey.

other stuff
15 min buffer: increased frame limit to 27,000 frames for XL levels.

low overhead: optimized the encoding thread to stop pc combustion.