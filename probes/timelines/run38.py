# reforce black-box probe run 38 (G-Force script, Python 2.7): which new sprite fades out a running one ("a new
# background sprite fades out the running one", [VH 3.6]). Run 37: a Pen full-frame newcomer does not replace a Pen
# full-frame sprite. Here EPen-only ("background-only" per the default params) old/new pairs. Void field. Sprites:
# build/probe-sprites38 = run 28's 'zz fd e' (bands, EPen 1, cover), 'zz fd p' (bands, Pen .5), 'zz bg' (flat 96,
# Pen 1, cover), 'zz f copy e' (native grid, EPen 1) plus 'zz bge' (flat 96, EPen 1, cover: 48 at full on black).
# PROBE_PREFS Sprite.Transition.Duration=2, deviation 0. White band of the old sprite: 127 at full.
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
@_time( '0:00' )
def e000(): vis.SetSlideshowEnabled( '*', False ); vis.KillRunningParticles( 0 ); vis.KillRunningSprites( 0 ); vis.SetConsoleVisible( False ); cm( 'zz reforce gray' ); ff( 'zz reforce void' ); ws( 'zz reforce one dot' )
@_time( '0:01' )
def e001(): sprite( 'zz fd e', 20 )
@_time( '0:04' )
def e002(): sprite( 'zz bge', 20 )
@_time( '0:04.30' )
def e003(): shot( 'fd e then bge +0.3' )   # 0001
@_time( '0:04.60' )
def e004(): shot( 'fd e then bge +0.6' )   # 0002
@_time( '0:05' )
def e005(): shot( 'fd e then bge +1.0' )   # 0003
@_time( '0:05.50' )
def e006(): shot( 'fd e then bge +1.5' )   # 0004
@_time( '0:06.50' )
def e007(): shot( 'fd e then bge +2.5' )   # 0005
@_time( '0:07.50' )
def e008(): vis.KillRunningSprites( 0 )
@_time( '0:08' )
def e009(): sprite( 'zz fd e', 20 )
@_time( '0:11' )
def e010(): sprite( 'zz f copy e', 20 )
@_time( '0:11.30' )
def e011(): shot( 'fd e then f copy e +0.3' )   # 0006
@_time( '0:11.60' )
def e012(): shot( 'fd e then f copy e +0.6' )   # 0007
@_time( '0:12' )
def e013(): shot( 'fd e then f copy e +1.0' )   # 0008
@_time( '0:12.50' )
def e014(): shot( 'fd e then f copy e +1.5' )   # 0009
@_time( '0:13.50' )
def e015(): shot( 'fd e then f copy e +2.5' )   # 0010
@_time( '0:14.50' )
def e016(): vis.KillRunningSprites( 0 )
@_time( '0:15' )
def e017(): sprite( 'zz fd e', 20 )
@_time( '0:18' )
def e018(): sprite( 'zz bg', 20 )
@_time( '0:18.30' )
def e019(): shot( 'fd e then bg +0.3' )   # 0011
@_time( '0:18.60' )
def e020(): shot( 'fd e then bg +0.6' )   # 0012
@_time( '0:19' )
def e021(): shot( 'fd e then bg +1.0' )   # 0013
@_time( '0:19.50' )
def e022(): shot( 'fd e then bg +1.5' )   # 0014
@_time( '0:20.50' )
def e023(): shot( 'fd e then bg +2.5' )   # 0015
@_time( '0:21.50' )
def e024(): vis.KillRunningSprites( 0 )
@_time( '0:22' )
def e025(): sprite( 'zz fd p', 20 )
@_time( '0:25' )
def e026(): sprite( 'zz bge', 20 )
@_time( '0:25.30' )
def e027(): shot( 'fd p then bge +0.3' )   # 0016
@_time( '0:25.60' )
def e028(): shot( 'fd p then bge +0.6' )   # 0017
@_time( '0:26' )
def e029(): shot( 'fd p then bge +1.0' )   # 0018
@_time( '0:26.50' )
def e030(): shot( 'fd p then bge +1.5' )   # 0019
@_time( '0:27.50' )
def e031(): shot( 'fd p then bge +2.5' )   # 0020
@_time( '0:28.50' )
def e032(): vis.KillRunningSprites( 0 )
@_time( '0:29' )
def e033(): print "reforce-probe: done"
