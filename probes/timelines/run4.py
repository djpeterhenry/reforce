# reforce black-box probe run 4 (G-Force script, Python 2.7): particle lifetime/fade-out, Union weight formula.
def ws( name, morph = 0 ):  vis.StartConfig( 'WaveShape', name, morph )
def cm( name, morph = 0 ):  vis.StartConfig( 'ColorMap', name, morph )
def ff( name, union = '', morph = 0 ):  vis.SetFlowField( name, union, morph, 0, True )
def shot( label ):
    print "reforce-probe shot: %s" % label
    vis.TakeScreenshot()

@_time( '0:00' )
def a000():
    vis.SetSlideshowEnabled( '*', False ); vis.KillRunningParticles( 0 ); vis.KillRunningSprites( 0 )
    vis.SetConsoleVisible( False )
    cm( 'zz reforce gray' ); ff( 'zz reforce void' ); ws( 'zz reforce one dot' )

# 1. Particle (PDur 4, requested 4 s) observed for 15 s on the void field.
@_time( '0:02' )
def a002(): vis.StartParticle( 'zz reforce pdot', 4 )
@_time( '0:05' )
def a005(): shot( 'p-03' )        # 0001
@_time( '0:07' )
def a007(): shot( 'p-05' )        # 0002
@_time( '0:09' )
def a009(): shot( 'p-07' )        # 0003
@_time( '0:11' )
def a011(): shot( 'p-09' )        # 0004
@_time( '0:13' )
def a013(): shot( 'p-11' )        # 0005
@_time( '0:15' )
def a015(): shot( 'p-13' )        # 0006
@_time( '0:17' )
def a017(): shot( 'p-15' )        # 0007

# 2. No-union transition identity -> zoom in over 6 s (reference), grid drawn once then held by identity.
@_time( '0:20' )
def a020(): ff( 'zz reforce identity' ); ws( 'zz reforce grid' )
@_time( '0:22' )
def a022(): ws( 'zz reforce one dot' ); ff( 'zz reforce zoom in', '', 6 )
@_time( '0:23.5' )
def a023(): shot( 'nou-25' )      # 0008
@_time( '0:25' )
def a025(): shot( 'nou-50' )      # 0009

# 3. Same with union ALFA = .5 everywhere.
@_time( '0:30' )
def a030(): ff( 'zz reforce identity' ); ws( 'zz reforce grid' )
@_time( '0:32' )
def a032(): ws( 'zz reforce one dot' ); ff( 'zz reforce zoom in', 'zz reforce union const', 6 )
@_time( '0:33.5' )
def a033(): shot( 'u5-25' )       # 0010
@_time( '0:35' )
def a035(): shot( 'u5-50' )       # 0011
@_time( '0:38.2' )
def a038(): shot( 'u5-end+0.2' )  # 0012
@_time( '0:40' )
def a040(): print "reforce-probe: done"
