# reforce black-box probe run24 (G-Force script, Python 2.7): F6 line width vs frame size. 'zz reforce widths' on the
# void field; a shot every 5 s. Between shots tools/parity/resize_gforce.sh resizes the G-Force window (it watches the
# capture folder: after shot k it sets size k+1, see SIZES there), then full screen for the last shot.
#   tools/parity/resize_gforce.sh & PROBE_TIMELINE=run24 PROBE_SECONDS=62 probes/run.sh --auto
# Run twice: default prefs (AutoLineScale 1) and PROBE_PREFS='WaveShape.AutoLineScale=0'.
def ws( name, morph = 0 ):  vis.StartConfig( 'WaveShape', name, morph )
def cm( name, morph = 0 ):  vis.StartConfig( 'ColorMap', name, morph )
def ff( name, union = '', morph = 0 ):  vis.SetFlowField( name, union, morph, 0, True )
def shot( label ):
    print "reforce-probe shot: %s" % label
    vis.TakeScreenshot()

@_time( '0:00' )
def a000():
    vis.SetSlideshowEnabled( '*', False ); vis.KillRunningParticles( 0 ); vis.KillRunningSprites( 0 )
    vis.SetConsoleVisible( False )
    cm( 'zz reforce gray' ); ff( 'zz reforce void' ); ws( 'zz reforce widths' )
@_time( '0:03' )
def s01(): shot( '01-size0' )
@_time( '0:08' )
def s02(): shot( '02-size1' )
@_time( '0:13' )
def s03(): shot( '03-size2' )
@_time( '0:18' )
def s04(): shot( '04-size3' )
@_time( '0:23' )
def s05(): shot( '05-size4' )
@_time( '0:28' )
def s06(): shot( '06-size5' )
@_time( '0:33' )
def s07(): shot( '07-size6' )
@_time( '0:38' )
def s08(): shot( '08-size7' )
@_time( '0:43' )
def s09(): shot( '09-size8' )
@_time( '0:46' )
def a046(): vis.SetFullscreen( True )
@_time( '0:50' )
def s10(): shot( '10-fullscreen' )
@_time( '0:52' )
def a052(): vis.SetFullscreen( False )
@_time( '0:55' )
def done(): print "reforce-probe: done"
