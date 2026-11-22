# G-Force 5.9.7 default preferences (measured)

How: on 2026-11-11 Peter's `Preferences (Standalone).txt` was moved aside, G-Force Standalone was launched and quit
normally, and the file it wrote was captured. Peter's prefs were then restored and verified identical to the backup.
These are the values G-Force uses with no prefs file. reforce's built-in defaults match them.

**Peter's file differs from these defaults in only three keys:** `Audio.InputSource = "DDJ-1000 Loopback"` (default: the
first input device found), `Audio.FFT.Smooth = 2` (default 1), `Audio.AutoDetect.Enabled = 0` (default 1).
Window position and usage counters are omitted.

```
"FlowField Union.ConfigPrepTime" = 3
"FlowField Union.Slideshow.EnableOnStartup" = 1
"FlowField Union.Slideshow.Interval.Duration" = 210
"FlowField Union.Slideshow.Interval.Duration.Deviation" = 0.1
"FlowField Union.Timescale" = 1
"FlowField Union.Transition.Duration" = 6
"FlowField Union.Transition.Duration.Deviation" = 0.02
"FlowField Union.Transition.Duration.Quick" = 0.9
"FlowField Union.Transition.Function" = "1-(1-t)^1.5"
Audio.AutoDetect.Enabled = 1
Audio.AutoDetect.SilenceThreshold = 0.0037
Audio.AutoDetect.Wait = 15
Audio.FFT.FreqHi = 0
Audio.FFT.FreqLo = 0
Audio.FFT.NumBins.in = 128
Audio.FFT.NumBins.vis = 255
Audio.FFT.Params = "FloorRaiseMax=.7 Envelopes=16 InitialVolume=1.0     ShortImpulse=0.60    ShortWindow=0.3    ShortRamp=44.0     LongImpulse=0.59   LongWindow=0.043  LongRamp=185.0  ExciterImpulse=1.2   PersistCoef=1.0   PersistLowThresh=.035"
Audio.FFT.Smooth = 1
Audio.PCM.NumBins = 255
Audio.PCM.Smooth = 0.008
Audio.Response.Scale = 1
ColorMap.ConfigPrepTime = 3
ColorMap.MapFunction = "i^2.1"
ColorMap.Slideshow.EnableOnStartup = 1
ColorMap.Slideshow.Interval.Duration = 15
ColorMap.Slideshow.Interval.Duration.Deviation = 0.1
ColorMap.Timescale = 1
ColorMap.Transition.Duration = 10
ColorMap.Transition.Duration.Deviation = 0.02
ColorMap.Transition.Duration.Quick = 0.9
ColorMap.Transition.Function = "1-(1-t)^1.5"
Console.LineDuration = 20
FlowField.ConfigPrepTime = 30
FlowField.NextFieldThread.Priority = -2
FlowField.NextFieldThread.SleepMS = 5
FlowField.Slideshow.EnableOnStartup = 1
FlowField.Slideshow.Interval.Duration = 20
FlowField.Slideshow.Interval.Duration.Deviation = 0.1
FlowField.Timescale = 1
FlowField.Transition.Duration = 10
FlowField.Transition.Duration.Deviation = 0.02
FlowField.Transition.Duration.Quick = 0.9
FlowField.Transition.Function = "1-(1-t)^1.35"
Fullscreen.Device = -1
G-Force.AutoLaunchToolbar = 1
Graphics.TargetFrameRate = 60
OS.PreventDisplaySleep = 0
Particle.ConfigPrepTime = 3
Particle.Duration = 25
Particle.Duration.Deviation = 0.2
Particle.Slideshow.EnableOnStartup = 1
Particle.Slideshow.Interval.Duration = 120
Particle.Slideshow.Interval.Duration.Deviation = 0.1
Particle.Timescale = 1
Particle.Transition.Duration = 6
Particle.Transition.Duration.Deviation = 0.02
Particle.Transition.Duration.Quick = 0.9
Particle.Transition.Function = "1-(1-t)^1.5"
Prefs.Autosave = 1
Prefs.Version = 8
Sprite.ConfigPrepTime = 3
Sprite.Duration = 45
Sprite.Duration.Deviation = 0.2
Sprite.Slideshow.EnableOnStartup = 1
Sprite.Slideshow.Interval.Duration = 240
Sprite.Slideshow.Interval.Duration.Deviation = 0.1
Sprite.Timescale = 1
Sprite.Transition.Duration = 6
Sprite.Transition.Duration.Deviation = 0.02
Sprite.Transition.Duration.Quick = 0.9
Sprite.Transition.Function = "1-(1-t)^1.5"
Theme.StartupTheme = "(Standard)"
TrackText.Animation = "(Random)"
TrackText.ChangeGracePeriod = 2.2
TrackText.Duration = 10
TrackText.FontName = ""
TrackText.Scale = 100
UI.ActivateOnChars = "\t\r "
UI.ActivateOnClick = 1
UI.AutoScaleFromHeight = 1200
UI.Enabled = 1
UI.FontName = ""
UI.Help.AutoShow = 1
UI.HideAfter = 15
UI.Scale = 1
UI.ShowOnStartup = 1
WaveShape.AntiAlias = 1
WaveShape.AutoLineScale = 1
WaveShape.ConfigPrepTime = 3
WaveShape.LineWidth.Offset = 0
WaveShape.LineWidth.Scale = 1
WaveShape.Slideshow.EnableOnStartup = 1
WaveShape.Slideshow.Interval.Duration = 25
WaveShape.Slideshow.Interval.Duration.Deviation = 0.1
WaveShape.Timescale = 1
WaveShape.Transition.Duration = 13
WaveShape.Transition.Duration.Deviation = 0.02
WaveShape.Transition.Duration.Quick = 0.9
WaveShape.Transition.Function = "1-(1-t)^1.6"
```
