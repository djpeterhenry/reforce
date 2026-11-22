# reforce black-box probe run 39 (G-Force script, Python 2.7): the fade-out of a running EPen-only sprite replaced by
# a new EPen-only sprite (run 38), sampled every 0.1 s. Old: 'zz fd e' (bands, cover; white band = 127.5 x envelope on
# the void field), new: 'zz f copy e' (native, does not overlap the band). Sprites: build/probe-sprites38. Run with
# different Sprite.Transition.Duration / .Quick to see which sets the fade-out.
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
def e001(): sprite( 'zz fd e', 30 )
@_time( '0:06' )
def e002(): sprite( 'zz f copy e', 30 )
@_time( '0:06.10' )
def e003(): shot( 'new +0.1' )   # 0001
@_time( '0:06.20' )
def e004(): shot( 'new +0.2' )   # 0002
@_time( '0:06.30' )
def e005(): shot( 'new +0.3' )   # 0003
@_time( '0:06.40' )
def e006(): shot( 'new +0.4' )   # 0004
@_time( '0:06.50' )
def e007(): shot( 'new +0.5' )   # 0005
@_time( '0:06.60' )
def e008(): shot( 'new +0.6' )   # 0006
@_time( '0:06.70' )
def e009(): shot( 'new +0.7' )   # 0007
@_time( '0:06.80' )
def e010(): shot( 'new +0.8' )   # 0008
@_time( '0:06.90' )
def e011(): shot( 'new +0.9' )   # 0009
@_time( '0:07' )
def e012(): shot( 'new +1.0' )   # 0010
@_time( '0:07.10' )
def e013(): shot( 'new +1.1' )   # 0011
@_time( '0:07.20' )
def e014(): shot( 'new +1.2' )   # 0012
@_time( '0:07.30' )
def e015(): shot( 'new +1.3' )   # 0013
@_time( '0:07.40' )
def e016(): shot( 'new +1.4' )   # 0014
@_time( '0:07.50' )
def e017(): shot( 'new +1.5' )   # 0015
@_time( '0:07.60' )
def e018(): shot( 'new +1.6' )   # 0016
@_time( '0:07.70' )
def e019(): shot( 'new +1.7' )   # 0017
@_time( '0:07.80' )
def e020(): shot( 'new +1.8' )   # 0018
@_time( '0:07.90' )
def e021(): shot( 'new +1.9' )   # 0019
@_time( '0:08' )
def e022(): shot( 'new +2.0' )   # 0020
@_time( '0:08.10' )
def e023(): shot( 'new +2.1' )   # 0021
@_time( '0:08.20' )
def e024(): shot( 'new +2.2' )   # 0022
@_time( '0:08.30' )
def e025(): shot( 'new +2.3' )   # 0023
@_time( '0:08.40' )
def e026(): shot( 'new +2.4' )   # 0024
@_time( '0:08.50' )
def e027(): shot( 'new +2.5' )   # 0025
@_time( '0:08.60' )
def e028(): shot( 'new +2.6' )   # 0026
@_time( '0:08.70' )
def e029(): shot( 'new +2.7' )   # 0027
@_time( '0:08.80' )
def e030(): shot( 'new +2.8' )   # 0028
@_time( '0:08.90' )
def e031(): shot( 'new +2.9' )   # 0029
@_time( '0:09' )
def e032(): shot( 'new +3.0' )   # 0030
@_time( '0:10' )
def e033(): vis.KillRunningSprites( 0 ); print "reforce-probe: done"
