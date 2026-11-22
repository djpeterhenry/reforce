# reforce black-box probe run 35 (G-Force script, Python 2.7): particle END_TIME - START_TIME (run 33/34 gave 10.1, 8.0,
# 10.6 for lives 16, 8, 13). PROBE_PREFS sets every particle deviation to 0 and Particle.Transition.Duration=2; repeated
# identical starts test for randomness; then Particle.Duration (pref) is varied per segment via the arg only.
# probes/Particle/zz reforce pdt (PDur 4): y 0 dot x = (END_TIME - START_TIME)/25 - .9. One shot per start at age 1.5.
def ws( name, morph = 0 ):  vis.StartConfig( 'WaveShape', name, morph )
def cm( name, morph = 0 ):  vis.StartConfig( 'ColorMap', name, morph )
def ff( name, union = '', morph = 0 ):  vis.SetFlowField( name, union, morph, 0, True )
def shot( label ):
    print "reforce-probe shot: %s" % label
    vis.TakeScreenshot()
@_time( '0:00' )
def e000(): vis.SetSlideshowEnabled( '*', False ); vis.KillRunningParticles( 0 ); vis.KillRunningSprites( 0 ); vis.SetConsoleVisible( False ); cm( 'zz reforce gray' ); ff( 'zz reforce void' ); ws( 'zz reforce one dot' )
@_time( '0:01' )
def e001(): vis.StartParticle( 'zz reforce pdt', 4 )
@_time( '0:02.50' )
def e002(): shot( 'arg 4' )   # 0001
@_time( '0:02.80' )
def e003(): vis.KillRunningParticles( 0 )
@_time( '0:03.30' )
def e004(): vis.StartParticle( 'zz reforce pdt', 4 )
@_time( '0:04.80' )
def e005(): shot( 'arg 4' )   # 0002
@_time( '0:05.10' )
def e006(): vis.KillRunningParticles( 0 )
@_time( '0:05.60' )
def e007(): vis.StartParticle( 'zz reforce pdt', 4 )
@_time( '0:07.10' )
def e008(): shot( 'arg 4' )   # 0003
@_time( '0:07.40' )
def e009(): vis.KillRunningParticles( 0 )
@_time( '0:07.90' )
def e010(): vis.StartParticle( 'zz reforce pdt', 9 )
@_time( '0:09.40' )
def e011(): shot( 'arg 9' )   # 0004
@_time( '0:09.70' )
def e012(): vis.KillRunningParticles( 0 )
@_time( '0:10.20' )
def e013(): vis.StartParticle( 'zz reforce pdt', 9 )
@_time( '0:11.70' )
def e014(): shot( 'arg 9' )   # 0005
@_time( '0:12' )
def e015(): vis.KillRunningParticles( 0 )
@_time( '0:12.50' )
def e016(): vis.StartParticle( 'zz reforce pdt', 9 )
@_time( '0:14' )
def e017(): shot( 'arg 9' )   # 0006
@_time( '0:14.30' )
def e018(): vis.KillRunningParticles( 0 )
@_time( '0:14.80' )
def e019(): vis.StartParticle( 'zz reforce pdt', 1 )
@_time( '0:16.30' )
def e020(): shot( 'arg 1' )   # 0007
@_time( '0:16.60' )
def e021(): vis.KillRunningParticles( 0 )
@_time( '0:17.10' )
def e022(): vis.StartParticle( 'zz reforce pdt', 2 )
@_time( '0:18.60' )
def e023(): shot( 'arg 2' )   # 0008
@_time( '0:18.90' )
def e024(): vis.KillRunningParticles( 0 )
@_time( '0:19.40' )
def e025(): vis.StartParticle( 'zz reforce pdt', 6 )
@_time( '0:20.90' )
def e026(): shot( 'arg 6' )   # 0009
@_time( '0:21.20' )
def e027(): vis.KillRunningParticles( 0 )
@_time( '0:21.70' )
def e028(): vis.StartParticle( 'zz reforce pdt', 15 )
@_time( '0:23.20' )
def e029(): shot( 'arg 15' )   # 0010
@_time( '0:23.50' )
def e030(): vis.KillRunningParticles( 0 )
@_time( '0:24' )
def e031(): vis.StartParticle( 'zz reforce pdt', 30 )
@_time( '0:25.50' )
def e032(): shot( 'arg 30' )   # 0011
@_time( '0:25.80' )
def e033(): vis.KillRunningParticles( 0 )
@_time( '0:26.30' )
def e034(): print "reforce-probe: done"
