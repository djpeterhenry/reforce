# reforce black-box probe run 36 (G-Force script, Python 2.7): which prefs enter particle END_TIME (run 35: END - START =
# 6 + arg/2 at Particle.Transition.Duration 2; run 33: 10 at 6 s, arg 4). Run three times with different PROBE_PREFS
# (fade 4; ConfigPrepTime 7; Particle.Duration 40). probes/Particle/zz reforce pdt, shot at age 1.5, args 4 and 10.
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
def e004(): vis.StartParticle( 'zz reforce pdt', 10 )
@_time( '0:04.80' )
def e005(): shot( 'arg 10' )   # 0002
@_time( '0:05.10' )
def e006(): vis.KillRunningParticles( 0 ); print "reforce-probe: done"
