# reforce black-box probe run 27 (G-Force script, Python 2.7): sprite X/Y per fit mode, and Pen/EPen combinations.
# Sprites from build/probe-sprites27 (tools/parity/make_sprite27.py) via G-Force.ExternalSprites.Path; run with
# PROBE_PREFS Sprite.Transition.Duration=2, deviation 0. Follow-up to run 26 (#40).
#  A. void field, no lines: each X/Y sprite alone at full envelope (EPen-only: 0.5 x luminance over black).
#  B. identity field + grid redrawn: Pen/EPen pairs at full, then 0.3 s after an instant kill (= what was fed back);
#     void then identity between pairs clears the leftover.
#  C. zoom field: a Pen-only sprite 1.0 and 1.5 s into its 2 s fade-in (how the envelope applies to Pen stamps).
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
def e000(): vis.SetSlideshowEnabled( '*', False ); vis.KillRunningParticles( 0 ); kill(); vis.SetConsoleVisible( False ); cm( 'zz reforce gray' ); ff( 'zz reforce void' ); ws( 'zz reforce one dot' )
@_time( '0:02' )
def e001(): sprite( 'zz x a5', 10 )
@_time( '0:05' )
def e002(): shot( 'zz x a5 full' )   # 0001
@_time( '0:05.50' )
def e003(): kill()
@_time( '0:06.50' )
def e004(): sprite( 'zz x w0', 10 )
@_time( '0:09.50' )
def e005(): shot( 'zz x w0 full' )   # 0002
@_time( '0:10' )
def e006(): kill()
@_time( '0:11' )
def e007(): sprite( 'zz x w1', 10 )
@_time( '0:14' )
def e008(): shot( 'zz x w1 full' )   # 0003
@_time( '0:14.50' )
def e009(): kill()
@_time( '0:15.50' )
def e010(): sprite( 'zz x w2', 10 )
@_time( '0:18.50' )
def e011(): shot( 'zz x w2 full' )   # 0004
@_time( '0:19' )
def e012(): kill()
@_time( '0:20' )
def e013(): sprite( 'zz x w10', 10 )
@_time( '0:23' )
def e014(): shot( 'zz x w10 full' )   # 0005
@_time( '0:23.50' )
def e015(): kill()
@_time( '0:24.50' )
def e016(): sprite( 'zz x n5', 10 )
@_time( '0:27.50' )
def e017(): shot( 'zz x n5 full' )   # 0006
@_time( '0:28' )
def e018(): kill()
@_time( '0:29' )
def e019(): sprite( 'zz x c', 10 )
@_time( '0:32' )
def e020(): shot( 'zz x c full' )   # 0007
@_time( '0:32.50' )
def e021(): kill()
@_time( '0:33.50' )
def e022(): ff( 'zz reforce identity' ); ws( 'zz reforce grid' )
@_time( '0:34' )
def e023(): sprite( 'zz pe 1-0', 10 )
@_time( '0:37' )
def e024(): shot( 'zz pe 1-0 full' )   # 0008
@_time( '0:37.50' )
def e025(): kill()
@_time( '0:37.80' )
def e026(): shot( 'zz pe 1-0 killed' )   # 0009
@_time( '0:38.20' )
def e027(): ff( 'zz reforce void' )
@_time( '0:38.70' )
def e028(): ff( 'zz reforce identity' )
@_time( '0:39' )
def e029(): sprite( 'zz pe 1-25', 10 )
@_time( '0:42' )
def e030(): shot( 'zz pe 1-25 full' )   # 0010
@_time( '0:42.50' )
def e031(): kill()
@_time( '0:42.80' )
def e032(): shot( 'zz pe 1-25 killed' )   # 0011
@_time( '0:43.20' )
def e033(): ff( 'zz reforce void' )
@_time( '0:43.70' )
def e034(): ff( 'zz reforce identity' )
@_time( '0:44' )
def e035(): sprite( 'zz pe 5-1', 10 )
@_time( '0:47' )
def e036(): shot( 'zz pe 5-1 full' )   # 0012
@_time( '0:47.50' )
def e037(): kill()
@_time( '0:47.80' )
def e038(): shot( 'zz pe 5-1 killed' )   # 0013
@_time( '0:48.20' )
def e039(): ff( 'zz reforce void' )
@_time( '0:48.70' )
def e040(): ff( 'zz reforce identity' )
@_time( '0:49' )
def e041(): sprite( 'zz pe 0-1', 10 )
@_time( '0:52' )
def e042(): shot( 'zz pe 0-1 full' )   # 0014
@_time( '0:52.50' )
def e043(): kill()
@_time( '0:52.80' )
def e044(): shot( 'zz pe 0-1 killed' )   # 0015
@_time( '0:53.20' )
def e045(): ff( 'zz reforce void' )
@_time( '0:53.70' )
def e046(): ff( 'zz reforce identity' )
@_time( '0:54' )
def e047(): ff( 'zz reforce zoom in' )
@_time( '0:54.50' )
def e048(): sprite( 'zz pe 1-x', 10 )
@_time( '0:55.50' )
def e049(): shot( 'zz pe 1-x fadein 1.0' )   # 0016
@_time( '0:56' )
def e050(): shot( 'zz pe 1-x fadein 1.5' )   # 0017
@_time( '0:57.50' )
def e051(): shot( 'zz pe 1-x full' )   # 0018
@_time( '0:58' )
def e052(): kill(); print "reforce-probe: done"
