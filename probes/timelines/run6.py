# reforce black-box probe run 6 (G-Force script, Python 2.7): line width vs render size (windowed vs fullscreen).
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
    cm( 'zz reforce gray' ); ff( 'zz reforce void' ); ws( 'zz reforce hlines' )
@_time( '0:02' )
def a002(): shot( '01-windowed' )            # 0001
@_time( '0:03' )
def a003(): vis.SetFullscreen( True )
@_time( '0:07' )
def a007(): shot( '02-fullscreen' )          # 0002
@_time( '0:08' )
def a008(): vis.SetFullscreen( False )
@_time( '0:11' )
def a011(): print "reforce-probe: done"
