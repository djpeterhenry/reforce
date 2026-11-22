# reforce black-box probe run 32 (G-Force script, Python 2.7): the sprite keying threshold during fades and at
# fractional pens (follow-up to #42). Void field (F = 0: keyed pixels show their value, skipped ones 0), WaveShape one
# dot. Sprites from build/probe-sprites32 (tools/parity/make_sprite32.py); PROBE_PREFS Sprite.Transition.Duration=2,
# deviation 0; StartSprite(name, 1): in 0-2 s, hold 2-3, out 3-5. The 255 column of each ramp gives the envelope of
# each shot, so screenshot timing does not matter.
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
@_time( '0:01' )
def e001(): sprite( 'zz k fade p', 1 )
@_time( '0:01.30' )
def e002(): shot( 'zz k fade p in 0.3' )   # 0001
@_time( '0:01.50' )
def e003(): shot( 'zz k fade p in 0.5' )   # 0002
@_time( '0:01.70' )
def e004(): shot( 'zz k fade p in 0.7' )   # 0003
@_time( '0:01.90' )
def e005(): shot( 'zz k fade p in 0.9' )   # 0004
@_time( '0:02.10' )
def e006(): shot( 'zz k fade p in 1.1' )   # 0005
@_time( '0:02.30' )
def e007(): shot( 'zz k fade p in 1.3' )   # 0006
@_time( '0:02.50' )
def e008(): shot( 'zz k fade p in 1.5' )   # 0007
@_time( '0:02.70' )
def e009(): shot( 'zz k fade p in 1.7' )   # 0008
@_time( '0:02.90' )
def e010(): shot( 'zz k fade p in 1.9' )   # 0009
@_time( '0:04.20' )
def e011(): shot( 'zz k fade p out 0.2' )   # 0010
@_time( '0:04.40' )
def e012(): shot( 'zz k fade p out 0.4' )   # 0011
@_time( '0:04.60' )
def e013(): shot( 'zz k fade p out 0.6' )   # 0012
@_time( '0:04.80' )
def e014(): shot( 'zz k fade p out 0.8' )   # 0013
@_time( '0:05' )
def e015(): shot( 'zz k fade p out 1.0' )   # 0014
@_time( '0:05.20' )
def e016(): shot( 'zz k fade p out 1.2' )   # 0015
@_time( '0:05.40' )
def e017(): shot( 'zz k fade p out 1.4' )   # 0016
@_time( '0:05.60' )
def e018(): shot( 'zz k fade p out 1.6' )   # 0017
@_time( '0:05.80' )
def e019(): shot( 'zz k fade p out 1.8' )   # 0018
@_time( '0:06.60' )
def e020(): sprite( 'zz k fade e', 1 )
@_time( '0:06.90' )
def e021(): shot( 'zz k fade e in 0.3' )   # 0019
@_time( '0:07.10' )
def e022(): shot( 'zz k fade e in 0.5' )   # 0020
@_time( '0:07.30' )
def e023(): shot( 'zz k fade e in 0.7' )   # 0021
@_time( '0:07.50' )
def e024(): shot( 'zz k fade e in 0.9' )   # 0022
@_time( '0:07.70' )
def e025(): shot( 'zz k fade e in 1.1' )   # 0023
@_time( '0:07.90' )
def e026(): shot( 'zz k fade e in 1.3' )   # 0024
@_time( '0:08.10' )
def e027(): shot( 'zz k fade e in 1.5' )   # 0025
@_time( '0:08.30' )
def e028(): shot( 'zz k fade e in 1.7' )   # 0026
@_time( '0:08.50' )
def e029(): shot( 'zz k fade e in 1.9' )   # 0027
@_time( '0:09.80' )
def e030(): shot( 'zz k fade e out 0.2' )   # 0028
@_time( '0:10' )
def e031(): shot( 'zz k fade e out 0.4' )   # 0029
@_time( '0:10.20' )
def e032(): shot( 'zz k fade e out 0.6' )   # 0030
@_time( '0:10.40' )
def e033(): shot( 'zz k fade e out 0.8' )   # 0031
@_time( '0:10.60' )
def e034(): shot( 'zz k fade e out 1.0' )   # 0032
@_time( '0:10.80' )
def e035(): shot( 'zz k fade e out 1.2' )   # 0033
@_time( '0:11' )
def e036(): shot( 'zz k fade e out 1.4' )   # 0034
@_time( '0:11.20' )
def e037(): shot( 'zz k fade e out 1.6' )   # 0035
@_time( '0:11.40' )
def e038(): shot( 'zz k fade e out 1.8' )   # 0036
@_time( '0:12.20' )
def e039(): sprite( 'zz k p3', 1 )
@_time( '0:14.70' )
def e040(): shot( 'zz k p3 full' )   # 0037
@_time( '0:17.60' )
def e041(): sprite( 'zz k e3', 1 )
@_time( '0:20.10' )
def e042(): shot( 'zz k e3 full' )   # 0038
@_time( '0:23' )
def e043(): sprite( 'zz k pe3', 1 )
@_time( '0:25.50' )
def e044(): shot( 'zz k pe3 full' )   # 0039
@_time( '0:28.40' )
def e045(): sprite( 'zz k dark', 1 )
@_time( '0:30.90' )
def e046(): shot( 'zz k dark full' )   # 0040
@_time( '0:33.80' )
def e047(): kill(); print "reforce-probe: done"
