# reforce black-box probe run 30 (G-Force script, Python 2.7): sprite keying for Pen < 1 and Pen 0, and how the fade
# envelope enters each blend mode. Sprites from build/probe-sprites28 (tools/parity/make_sprite28.py);
# PROBE_PREFS Sprite.Transition.Duration=2, deviation 0. WaveShape one dot.
#  A. identity field, test sprite first, flat-96 background 0.05 s later (older draws on top, run 29): Pen .5 and
#     Pen 0 over the low-luminance ramp.
#  B. identity field, background running first (EPen draws are display-only, order irrelevant): EPen-only sprites
#     1.0 s into the 2 s fade-in (envelope .646) per blend mode.
#  C. void field (F = 0): low ramp Pen 1 and ThresholdCopy EPen 1, 1.0 s into the fade-in and at full.
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
def e001(): sprite( 'zz t copy p5', 3 )
@_time( '0:01.05' )
def e002(): sprite( 'zz bg2', 3 )
@_time( '0:03.60' )
def e003(): shot( 'zz t copy p5 full (bg after)' )   # 0001
@_time( '0:04.20' )
def e004(): kill()
@_time( '0:04.60' )
def e005(): sprite( 'zz t copy p0', 3 )
@_time( '0:04.65' )
def e006(): sprite( 'zz bg2', 3 )
@_time( '0:07.20' )
def e007(): shot( 'zz t copy p0 full (bg after)' )   # 0002
@_time( '0:07.80' )
def e008(): kill()
@_time( '0:08.20' )
def e009(): sprite( 'zz bg', 300 )
@_time( '0:11.20' )
def e010(): sprite( 'zz f copy e', 1 )
@_time( '0:12.20' )
def e011(): shot( 'zz f copy e fadein 1.0' )   # 0003
@_time( '0:16.50' )
def e012(): sprite( 'zz f add e', 1 )
@_time( '0:17.50' )
def e013(): shot( 'zz f add e fadein 1.0' )   # 0004
@_time( '0:21.80' )
def e014(): sprite( 'zz f am e', 1 )
@_time( '0:22.80' )
def e015(): shot( 'zz f am e fadein 1.0' )   # 0005
@_time( '0:27.10' )
def e016(): sprite( 'zz f ab e', 1 )
@_time( '0:28.10' )
def e017(): shot( 'zz f ab e fadein 1.0' )   # 0006
@_time( '0:32.40' )
def e018(): sprite( 'zz f thr e', 1 )
@_time( '0:33.40' )
def e019(): shot( 'zz f thr e fadein 1.0' )   # 0007
@_time( '0:37.70' )
def e020(): kill()
@_time( '0:38' )
def e021(): ff( 'zz reforce void' )
@_time( '0:38.70' )
def e022(): sprite( 'zz t copy p', 1 )
@_time( '0:39.70' )
def e023(): shot( 'zz t copy p fadein 1.0 (void)' )   # 0008
@_time( '0:41.20' )
def e024(): shot( 'zz t copy p full (void)' )   # 0009
@_time( '0:44' )
def e025(): sprite( 'zz t thr e', 1 )
@_time( '0:45' )
def e026(): shot( 'zz t thr e fadein 1.0 (void)' )   # 0010
@_time( '0:46.50' )
def e027(): shot( 'zz t thr e full (void)' )   # 0011
@_time( '0:49.30' )
def e028(): kill(); print "reforce-probe: done"
