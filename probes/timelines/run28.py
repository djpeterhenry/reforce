# reforce black-box probe run 28 (G-Force script, Python 2.7): sprite blend flags, alpha, keying threshold,
# EPen scaling, colour weights, and the Pen / Pen+EPen / EPen fade envelopes. Follow-up to runs 26/27 (#40/#41).
# Sprites from build/probe-sprites28 (tools/parity/make_sprite28.py) via G-Force.ExternalSprites.Path; run with
# PROBE_PREFS Sprite.Transition.Duration=2, deviation 0 (StartSprite(name, 1): in 0-2 s, hold 2-3, out 3-5).
#  A. identity field, WaveShape one dot, a full-frame flat-96 Pen sprite ('zz bg') restamped every frame, so every
#     test sprite (native 660x420 lum x alpha grid in the centre) draws over F = 96; one shot each at full.
#  B. void field (nothing fed back): fade-in 1.0 / 1.5 s, full, 1.0 s into the fade-out for Pen .5, Pen .5 +
#     EPen 1, and EPen 1 alone.
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
def e001(): sprite( 'zz bg', 400 )
@_time( '0:04' )
def e002(): shot( 'bg alone' )   # 0001
@_time( '0:04.50' )
def e003(): sprite( 'zz f copy p', 1 )
@_time( '0:07' )
def e004(): shot( 'zz f copy p full' )   # 0002
@_time( '0:09.80' )
def e005(): shot( 'zz f copy p ended (bg restamped?)' )   # 0003
@_time( '0:09.90' )
def e006(): sprite( 'zz f copy e', 1 )
@_time( '0:12.40' )
def e007(): shot( 'zz f copy e full' )   # 0004
@_time( '0:15.30' )
def e008(): sprite( 'zz f thr p', 1 )
@_time( '0:17.80' )
def e009(): shot( 'zz f thr p full' )   # 0005
@_time( '0:20.70' )
def e010(): sprite( 'zz f thr e', 1 )
@_time( '0:23.20' )
def e011(): shot( 'zz f thr e full' )   # 0006
@_time( '0:26.10' )
def e012(): sprite( 'zz f add p', 1 )
@_time( '0:28.60' )
def e013(): shot( 'zz f add p full' )   # 0007
@_time( '0:31.50' )
def e014(): sprite( 'zz f add e', 1 )
@_time( '0:34' )
def e015(): shot( 'zz f add e full' )   # 0008
@_time( '0:36.90' )
def e016(): sprite( 'zz f sel p', 1 )
@_time( '0:39.40' )
def e017(): shot( 'zz f sel p full' )   # 0009
@_time( '0:42.30' )
def e018(): sprite( 'zz f sel e', 1 )
@_time( '0:44.80' )
def e019(): shot( 'zz f sel e full' )   # 0010
@_time( '0:47.70' )
def e020(): sprite( 'zz f am p', 1 )
@_time( '0:50.20' )
def e021(): shot( 'zz f am p full' )   # 0011
@_time( '0:53.10' )
def e022(): sprite( 'zz f am e', 1 )
@_time( '0:55.60' )
def e023(): shot( 'zz f am e full' )   # 0012
@_time( '0:58.50' )
def e024(): sprite( 'zz f ab p', 1 )
@_time( '1:01' )
def e025(): shot( 'zz f ab p full' )   # 0013
@_time( '1:03.90' )
def e026(): sprite( 'zz f ab e', 1 )
@_time( '1:06.40' )
def e027(): shot( 'zz f ab e full' )   # 0014
@_time( '1:09.30' )
def e028(): sprite( 'zz f inv p', 1 )
@_time( '1:11.80' )
def e029(): shot( 'zz f inv p full' )   # 0015
@_time( '1:14.70' )
def e030(): sprite( 'zz f inv e', 1 )
@_time( '1:17.20' )
def e031(): shot( 'zz f inv e full' )   # 0016
@_time( '1:20.10' )
def e032(): sprite( 'zz f add p5', 1 )
@_time( '1:22.60' )
def e033(): shot( 'zz f add p5 full' )   # 0017
@_time( '1:25.50' )
def e034(): sprite( 'zz f ab p5', 1 )
@_time( '1:28' )
def e035(): shot( 'zz f ab p5 full' )   # 0018
@_time( '1:30.90' )
def e036(): sprite( 'zz f copy e5', 1 )
@_time( '1:33.40' )
def e037(): shot( 'zz f copy e5 full' )   # 0019
@_time( '1:36.30' )
def e038(): sprite( 'zz f copy pe', 1 )
@_time( '1:38.80' )
def e039(): shot( 'zz f copy pe full' )   # 0020
@_time( '1:41.70' )
def e040(): sprite( 'zz f col p', 1 )
@_time( '1:44.20' )
def e041(): shot( 'zz f col p full' )   # 0021
@_time( '1:47.10' )
def e042(): kill()
@_time( '1:47.60' )
def e043(): ff( 'zz reforce void' )
@_time( '1:48.60' )
def e044(): sprite( 'zz fd p', 1 )
@_time( '1:49.60' )
def e045(): shot( 'zz fd p fadein 1.0' )   # 0022
@_time( '1:50.10' )
def e046(): shot( 'zz fd p fadein 1.5' )   # 0023
@_time( '1:51.10' )
def e047(): shot( 'zz fd p full' )   # 0024
@_time( '1:52.60' )
def e048(): shot( 'zz fd p fadeout 1.0' )   # 0025
@_time( '1:54.20' )
def e049(): sprite( 'zz fd pe', 1 )
@_time( '1:55.20' )
def e050(): shot( 'zz fd pe fadein 1.0' )   # 0026
@_time( '1:55.70' )
def e051(): shot( 'zz fd pe fadein 1.5' )   # 0027
@_time( '1:56.70' )
def e052(): shot( 'zz fd pe full' )   # 0028
@_time( '1:58.20' )
def e053(): shot( 'zz fd pe fadeout 1.0' )   # 0029
@_time( '1:59.80' )
def e054(): sprite( 'zz fd e', 1 )
@_time( '2:00.80' )
def e055(): shot( 'zz fd e fadein 1.0' )   # 0030
@_time( '2:01.30' )
def e056(): shot( 'zz fd e fadein 1.5' )   # 0031
@_time( '2:02.30' )
def e057(): shot( 'zz fd e full' )   # 0032
@_time( '2:03.80' )
def e058(): shot( 'zz fd e fadeout 1.0' )   # 0033
@_time( '2:05.40' )
def e059(): kill(); print "reforce-probe: done"
