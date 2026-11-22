# reforce run24 helper (G-Force script, Python 2.7): toggles full screen once, nothing else. G-Force remembers the
# window rect it had when full screen was last entered and reopens there on the next launch (not stored in the
# SoundSpectrum prefs that run.sh restores). After run24 resized the window, put the window back to Peter's rect via
# System Events before 0:10, then this timeline makes G-Force remember that rect again. See tools/parity/resize_gforce.sh.
@_time( '0:00' )
def a000(): vis.SetConsoleVisible( False )
@_time( '0:10' )
def a010(): vis.SetFullscreen( True )
@_time( '0:14' )
def a014(): vis.SetFullscreen( False )
@_time( '0:17' )
def done(): print "reforce-probe: done"
