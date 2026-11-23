# reforce black-box probe run 44 (G-Force script, Python 2.7): does a sprite's own .txt replace the package's
# _default_sprite_params.txt (EPen 1, X/Y 0) or merge with it? build/probe-sprites44/'zz m pen': the run 26 band image
# (600x382) with a .txt holding only Pen=".5". Void field. Merged: EPen 1 replaces the display (white band 255);
# alone: the Pen stamp shows (127). Fit (no Flag): cover (full frame) or native (600x382 in the center).
# PROBE_PREFS Sprite.Transition.Duration=2, deviation 0.
def ws( name, morph = 0 ):  vis.StartConfig( 'WaveShape', name, morph )
def cm( name, morph = 0 ):  vis.StartConfig( 'ColorMap', name, morph )
def ff( name, union = '', morph = 0 ):  vis.SetFlowField( name, union, morph, 0, True )
def shot( label ):
    print "reforce-probe shot: %s" % label
    vis.TakeScreenshot()
@_time( '0:00' )
def e000(): vis.SetSlideshowEnabled( '*', False ); vis.KillRunningParticles( 0 ); vis.KillRunningSprites( 0 ); vis.SetConsoleVisible( False ); cm( 'zz reforce gray' ); ff( 'zz reforce void' ); ws( 'zz reforce one dot' )
@_time( '0:01' )
def e001(): vis.StartSprite( 'zz m pen', 10 )
@_time( '0:04' )
def e002(): shot( 'm pen full' )   # 0001
@_time( '0:05' )
def e003(): vis.KillRunningSprites( 0 ); print "reforce-probe: done"
