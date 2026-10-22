# Run from the repository root: vhs docs/assets/start-screen/start-screen.tape
Output docs/assets/start-screen.gif

Set Shell "bash"
Set FontSize 14
Set Width 900
Set Height 600
Set Padding 0
Set Theme "Dracula"
Set Framerate 20
Set PlaybackSpeed 1

Hide
Type "sh docs/assets/start-screen/run.sh"
Enter
Show
Sleep 9s
Hide
Ctrl+C
Sleep 500ms
