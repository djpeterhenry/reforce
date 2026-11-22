# reforce black-box probe run 5 (G-Force script, Python 2.7): sprites (external sprite folder), Union end-of-transition,
# line width at a different window size (run with Window.* prefs changed).
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
def a002(): shot( '01-hlines-size' )                      # 0001 line widths at the new window size

# 2. Sprite over a held grid (identity field): fit, fade, does it wipe the grid?
@_time( '0:03' )
def a003(): ff( 'zz reforce identity' ); ws( 'zz reforce grid' )
@_time( '0:05' )
def a005(): ws( 'zz reforce one dot' ); vis.StartSprite( 'zz reforce sprite', 6 )
@_time( '0:06' )
def a006(): shot( '02-sprite-1s' )                        # 0002
@_time( '0:08' )
def a008(): shot( '02-sprite-3s' )                        # 0003
@_time( '0:11' )
def a011(): shot( '02-sprite-6s' )                        # 0004
@_time( '0:14' )
def a014(): shot( '02-sprite-9s' )                        # 0005
@_time( '0:18' )
def a018(): shot( '02-sprite-13s' )                       # 0006
@_time( '0:24' )
def a024(): shot( '02-sprite-19s' )                       # 0007

# 3. Union end-of-transition: identity -> zoom in, ALFA = (x > 0), 6 s.
@_time( '0:26' )
def a026(): vis.KillRunningSprites( 0 ); ff( 'zz reforce identity' ); ws( 'zz reforce grid' )
@_time( '0:28' )
def a028(): ws( 'zz reforce one dot' ); ff( 'zz reforce zoom in', 'zz reforce union half', 6 )
@_time( '0:33.7' )
def a033(): shot( '03-union-5.7s' )                       # 0008
@_time( '0:34.3' )
def a034(): shot( '03-union-6.3s' )                       # 0009
@_time( '0:35' )
def a035(): shot( '03-union-7.0s' )                       # 0010
@_time( '0:37' )
def a037(): print "reforce-probe: done"
