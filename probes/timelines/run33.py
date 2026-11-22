# reforce black-box probe run 33 (G-Force script, Python 2.7): dt in a plain WaveShape (probes/WaveShape/zz reforce dt:
# dots at x = dt * k - .9 for four scales, and a dt == 0 test dot), then a particle's dt, END_TIME - START_TIME and
# t - START_TIME (probes/Particle/zz reforce pdt, PDur 4, Particle.Transition.Duration default 6 s) at ages 1-15 s.
# Void field (no trails), gray ColorMap.
def ws( name, morph = 0 ):  vis.StartConfig( 'WaveShape', name, morph )
def cm( name, morph = 0 ):  vis.StartConfig( 'ColorMap', name, morph )
def ff( name, union = '', morph = 0 ):  vis.SetFlowField( name, union, morph, 0, True )
def shot( label ):
    print "reforce-probe shot: %s" % label
    vis.TakeScreenshot()
@_time( '0:00' )
def e000(): vis.SetSlideshowEnabled( '*', False ); vis.KillRunningParticles( 0 ); vis.KillRunningSprites( 0 ); vis.SetConsoleVisible( False ); cm( 'zz reforce gray' ); ff( 'zz reforce void' ); ws( 'zz reforce dt' )
@_time( '0:01' )
def e001(): shot( 'ws dt 1 s' )   # 0001
@_time( '0:03' )
def e002(): shot( 'ws dt 3 s' )   # 0002
@_time( '0:06' )
def e003(): shot( 'ws dt 6 s' )   # 0003
@_time( '0:07' )
def e004(): ws( 'zz reforce one dot' )
@_time( '0:08' )
def e005(): vis.StartParticle( 'zz reforce pdt', 4 )
@_time( '0:09' )
def e006(): shot( 'particle age 1' )   # 0004
@_time( '0:11' )
def e007(): shot( 'particle age 3' )   # 0005
@_time( '0:13' )
def e008(): shot( 'particle age 5' )   # 0006
@_time( '0:15' )
def e009(): shot( 'particle age 7' )   # 0007
@_time( '0:17' )
def e010(): shot( 'particle age 9' )   # 0008
@_time( '0:19' )
def e011(): shot( 'particle age 11' )   # 0009
@_time( '0:21' )
def e012(): shot( 'particle age 13' )   # 0010
@_time( '0:23' )
def e013(): shot( 'particle age 15' )   # 0011
@_time( '0:25' )
def e020(): vis.KillRunningParticles( 0 ); print "reforce-probe: done"
