v1.0.2 - quality of life
new features
fixed bugs, thats it

ui changes
added a confirmation popup when deleting clips to prevent accidental deletion.
added a hint in the gallery for new users on how to record their first clip.
compleatly revampted the ui. was too bad

v1.0.1 - declouded build
technical changes
removed all Google Drive / cloud upload code and dependencies. clips are now saved strictly to local storage.

ui changes
cleaned gallery footer (removed drive connect/reconnect button and status text) so the UI only reflects local clips. 

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
