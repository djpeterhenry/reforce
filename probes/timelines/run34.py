# reforce black-box probe run 34 (G-Force script, Python 2.7): particle END_TIME and dt with a 2 s fade
# (PROBE_PREFS Particle.Transition.Duration=2, deviation 0): run 33 gave END_TIME - START_TIME = 10 = fade 6 + PDur 4 and
# dt = age / 16 (the whole life). Expect 6 and age / 8 here. Then StartParticle( name, 9 ) on a PDur=4 config: which wins?
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
@_time( '0:02' )
def e002(): shot( 'arg 4 age 1' )   # 0001
@_time( '0:04' )
def e003(): shot( 'arg 4 age 3' )   # 0002
@_time( '0:06' )
def e004(): shot( 'arg 4 age 5' )   # 0003
@_time( '0:07.50' )
def e005(): shot( 'arg 4 age 6.5' )   # 0004
@_time( '0:10' )
def e006(): vis.StartParticle( 'zz reforce pdt', 9 )
@_time( '0:11' )
def e007(): shot( 'arg 9 age 1' )   # 0005
@_time( '0:15' )
def e008(): shot( 'arg 9 age 5' )   # 0006
@_time( '0:19' )
def e009(): shot( 'arg 9 age 9' )   # 0007
@_time( '0:22' )
def e010(): shot( 'arg 9 age 12' )   # 0008
@_time( '0:24' )
def e011(): vis.KillRunningParticles( 0 ); print "reforce-probe: done"
