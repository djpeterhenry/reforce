# reforce black-box probe run19 (G-Force script, Python 2.7): 5B sub-pixel lines and dots (LWdt .05-.5) on the void field: one frame per shot.
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
    cm( 'zz reforce gray' ); ff( 'zz reforce void' ); ws( 'zz reforce thin lines' )
@_time( '0:02' )
def s001(): shot( '01-thin-lines' )        # 0001
@_time( '0:02.5' )
def s002(): shot( '02-thin-lines' )        # 0002
@_time( '0:03' )
def a003(): ws( 'zz reforce thin dots' )
@_time( '0:05' )
def s003(): shot( '03-thin-dots' )        # 0003
@_time( '0:05.5' )
def s004(): shot( '04-thin-dots' )        # 0004
@_time( '0:07' )
def done(): print "reforce-probe: done"
