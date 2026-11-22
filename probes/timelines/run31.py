# reforce black-box probe run 31 (G-Force script, Python 2.7): does Pen 0 with EPen 1 erase the feedback under the
# image? (Run 27's "Pen 0 erases" was measured over an already-black buffer; run 30: Pen 0 alone draws nothing.)
# Identity field, WaveShape one dot. Test sprite first, flat-96 background 0.05 s later (the older sprite draws on
# top, run 29), instant kill of both, shot 0.1 s later = the fed-back buffer (~6 levels of decay). Control: Pen 1.
# Sprites from build/probe-sprites28 (tools/parity/make_sprite28.py); PROBE_PREFS Sprite.Transition.Duration=2.
def ws( name, morph = 0 ):  vis.StartConfig( 'WaveShape', name, morph )
def cm( name, morph = 0 ):  vis.StartConfig( 'ColorMap', name, morph )
def ff( name, union = '', morph = 0 ):  vis.SetFlowField( name, union, morph, 0, True )
def shot( label ):
    print "reforce-probe shot: %s" % label
    vis.TakeScreenshot()
def sprite( name, dur ):
    try:
        vis.StartSprite( name, dur )
        print "reforce-probe: StartSprite(%r, %r) ok" % ( name, dur )
    except Exception, e:
        print "reforce-probe: StartSprite(%r) failed: %s" % ( name, e )
def kill():  vis.KillRunningSprites( 0 )
@_time( '0:00' )
def e000(): vis.SetSlideshowEnabled( '*', False ); vis.KillRunningParticles( 0 ); kill(); vis.SetConsoleVisible( False ); cm( 'zz reforce gray' ); ff( 'zz reforce identity' ); ws( 'zz reforce one dot' )
@_time( '0:01' )
def e001(): sprite( 'zz f pe 0-1', 3 )
@_time( '0:01.05' )
def e002(): sprite( 'zz bg2', 3 )
@_time( '0:03.60' )
def e003(): shot( 'zz f pe 0-1 full' )   # 0001
@_time( '0:03.80' )
def e004(): kill()
@_time( '0:03.90' )
def e005(): shot( 'zz f pe 0-1 killed 0.1' )   # 0002
@_time( '0:05' )
def e006(): sprite( 'zz f copy p', 3 )
@_time( '0:05.05' )
def e007(): sprite( 'zz bg2', 3 )
@_time( '0:07.60' )
def e008(): shot( 'zz f copy p full' )   # 0003
@_time( '0:07.80' )
def e009(): kill()
@_time( '0:07.90' )
def e010(): shot( 'zz f copy p killed 0.1' )   # 0004
@_time( '0:09' )
def e011(): kill(); print "reforce-probe: done"
