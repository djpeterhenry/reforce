# reforce black-box probe run20 (G-Force script, Python 2.7): 5B.5 ColorMap.MapFunction LUT domain. Run with PROBE_PREFS='ColorMap.MapFunction="i^2.1"' (the
# default): 256-dot Pen ramp read through the gray ColorMap, then through 'zz reforce clamp' (R = 2i, G = -1 + i).
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
    cm( 'zz reforce gray' ); ff( 'zz reforce void' ); ws( 'zz reforce sweep ramp' )
@_time( '0:02' )
def s001(): shot( '01-ramp-gray' )        # 0001
@_time( '0:02.5' )
def s002(): shot( '02-ramp-gray' )        # 0002
@_time( '0:03' )
def a003(): cm( 'zz reforce clamp' )
@_time( '0:05' )
def s003(): shot( '03-ramp-clamp' )        # 0003
@_time( '0:05.5' )
def s004(): shot( '04-ramp-clamp' )        # 0004
@_time( '0:07' )
def done(): print "reforce-probe: done"
