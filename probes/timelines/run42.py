# reforce black-box probe run 42 (G-Force script, Python 2.7): how long a FlowField Union keeps shaping the field (run 41:
# with ALFA .5 the zoom was still ~1.014/frame 1 s after the 4 s morph's end). Circle trail on identity -> zoom in with
# 'zz reforce union const' (ALFA .5), morph 4 s; shots from 1 to 12 s after the start; the newest part of the trail
# gives the current per-frame zoom (1.03 = new field everywhere, 1.015 = ALFA-weighted). Run with and without
# "FlowField Union.Transition.Duration"=2.
def ws( name, morph = 0 ):  vis.StartConfig( 'WaveShape', name, morph )
def cm( name, morph = 0 ):  vis.StartConfig( 'ColorMap', name, morph )
def ff( name, union = '', morph = 0 ):  vis.SetFlowField( name, union, morph, 0, True )
def shot( label ):
    print "reforce-probe shot: %s" % label
    vis.TakeScreenshot()
@_time( '0:00' )
def e000(): vis.SetSlideshowEnabled( '*', False ); vis.KillRunningParticles( 0 ); vis.KillRunningSprites( 0 ); vis.SetConsoleVisible( False ); cm( 'zz reforce gray' ); ff( 'zz reforce identity' ); ws( 'zz reforce circle' )
@_time( '0:02' )
def e001(): ff( 'zz reforce zoom in', 'zz reforce union const', 4 )
@_time( '0:03' )
def e002(): shot( 'union +1.0' )   # 0001
@_time( '0:04' )
def e003(): shot( 'union +2.0' )   # 0002
@_time( '0:05' )
def e004(): shot( 'union +3.0' )   # 0003
@_time( '0:06' )
def e005(): shot( 'union +4.0' )   # 0004
@_time( '0:06.50' )
def e006(): shot( 'union +4.5' )   # 0005
@_time( '0:07' )
def e007(): shot( 'union +5.0' )   # 0006
@_time( '0:08' )
def e008(): shot( 'union +6.0' )   # 0007
@_time( '0:09' )
def e009(): shot( 'union +7.0' )   # 0008
@_time( '0:10' )
def e010(): shot( 'union +8.0' )   # 0009
@_time( '0:12' )
def e011(): shot( 'union +10.0' )   # 0010
@_time( '0:14' )
def e012(): shot( 'union +12.0' )   # 0011
@_time( '0:15' )
def e013(): print "reforce-probe: done"
