# reforce black-box probe run 37 (G-Force script, Python 2.7): how a running full-frame sprite leaves when a new
# full-frame ("background") sprite starts ([VH 3.6]), and whether a native-size sprite replaces it too. Void field
# (F = 0). Sprites from build/probe-sprites28: 'zz fd p' (bands, Pen .5, cover: white band 127 at full), 'zz bg'
# (flat 96, Pen 1, cover), 'zz t copy p' (native low ramp, Pen 1). PROBE_PREFS Sprite.Transition.Duration=2, deviation 0.
# The older sprite draws on top (#42), so the band keeps showing the old sprite while it is above ~10 levels.
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
def e001(): sprite( 'zz fd p', 20 )
@_time( '0:03.80' )
def e002(): shot( 'old alone' )   # 0001
@_time( '0:04' )
def e003(): sprite( 'zz bg', 20 )
@_time( '0:04.15' )
def e004(): shot( 'bg +0.15' )   # 0002
@_time( '0:04.30' )
def e005(): shot( 'bg +0.30' )   # 0003
@_time( '0:04.45' )
def e006(): shot( 'bg +0.45' )   # 0004
@_time( '0:04.60' )
def e007(): shot( 'bg +0.60' )   # 0005
@_time( '0:04.80' )
def e008(): shot( 'bg +0.80' )   # 0006
@_time( '0:05.00' )
def e009(): shot( 'bg +1.00' )   # 0007
@_time( '0:05.25' )
def e010(): shot( 'bg +1.25' )   # 0008
@_time( '0:05.50' )
def e011(): shot( 'bg +1.50' )   # 0009
@_time( '0:06.00' )
def e012(): shot( 'bg +2.00' )   # 0010
@_time( '0:06.50' )
def e013(): shot( 'bg +2.50' )   # 0011
@_time( '0:07.00' )
def e014(): shot( 'bg +3.00' )   # 0012
@_time( '0:08' )
def e015(): vis.KillRunningSprites( 0 )
@_time( '0:09' )
def e016(): sprite( 'zz fd p', 20 )
@_time( '0:12' )
def e017(): sprite( 'zz t copy p', 20 )
@_time( '0:12.50' )
def e018(): shot( 'native +0.5' )   # 0013
@_time( '0:13.50' )
def e019(): shot( 'native +1.5' )   # 0014
@_time( '0:15.00' )
def e020(): shot( 'native +3.0' )   # 0015
@_time( '0:16' )
def e021(): vis.KillRunningSprites( 0 ); print "reforce-probe: done"
