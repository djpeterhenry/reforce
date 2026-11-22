# reforce black-box probe run 3 (G-Force script, Python 2.7). Run with Graphics.TargetFrameRate = 30.
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
    cm( 'zz reforce gray' ); ff( 'zz reforce identity' ); ws( 'zz reforce circle' )

# 1. Decay at 30 fps (compare run 1 at 60 fps: 255,228,174,63 at +.5,+1.5,+3.5 s)
@_time( '0:03' )
def a003(): ws( 'zz reforce flash' )
@_time( '0:04' )
def a004(): shot( '01-decay30-1.0s' )          # 0001
@_time( '0:05' )
def a005(): shot( '01-decay30-2.0s' )          # 0002
@_time( '0:07' )
def a007(): shot( '01-decay30-4.0s' )          # 0003

# 2. Dilation of an unmatched path off-axis: two dots -> one dot over 10 s, void field (no trails)
@_time( '0:09' )
def a009(): ff( 'zz reforce void' ); ws( 'zz reforce two dots' )
@_time( '0:11' )
def a011(): ws( 'zz reforce one dot', 10 )
@_time( '0:11.5' )
def a011a(): shot( '02-dil-05' )              # 0004
@_time( '0:12' )
def a012(): shot( '02-dil-10' )               # 0005
@_time( '0:12.5' )
def a012a(): shot( '02-dil-15' )              # 0006
@_time( '0:13' )
def a013(): shot( '02-dil-20' )               # 0007
@_time( '0:13.5' )
def a013a(): shot( '02-dil-25' )              # 0008
@_time( '0:14' )
def a014(): shot( '02-dil-30' )               # 0009
@_time( '0:14.5' )
def a014a(): shot( '02-dil-35' )              # 0010

# 3. Union: identity -> zoom in over 6 s with ALFA = (x > 0)
@_time( '0:22' )
def a022(): ff( 'zz reforce identity' ); ws( 'zz reforce grid' )
@_time( '0:24' )
def a024(): ws( 'zz reforce one dot' ); ff( 'zz reforce zoom in', 'zz reforce union half', 6 )
@_time( '0:25.5' )
def a025(): shot( '03-union-25' )             # 0011
@_time( '0:27' )
def a027(): shot( '03-union-50' )             # 0012
@_time( '0:33' )
def a033(): shot( '03-union-after' )          # 0013  (3 s after the end)

# 4. Particle fade envelope (PDur 4 s), void field, no WaveShape dot in the way
@_time( '0:35' )
def a035(): ff( 'zz reforce void' ); ws( 'zz reforce one dot' ); vis.StartParticle( 'zz reforce pdot', 4 )
@_time( '0:35.25' )
def a035a(): shot( '04-p-0.25' )              # 0014
@_time( '0:35.75' )
def a035b(): shot( '04-p-0.75' )              # 0015
@_time( '0:36.5' )
def a036(): shot( '04-p-1.5' )                # 0016
@_time( '0:37.5' )
def a037(): shot( '04-p-2.5' )                # 0017
@_time( '0:38.25' )
def a038(): shot( '04-p-3.25' )               # 0018
@_time( '0:38.75' )
def a038b(): shot( '04-p-3.75' )              # 0019
@_time( '0:39.5' )
def a039(): shot( '04-p-4.5' )                # 0020
@_time( '0:41' )
def a041(): print "reforce-probe: done"
