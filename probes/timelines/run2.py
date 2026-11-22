# reforce black-box probe run 2 (G-Force script, Python 2.7). See docs/probe-results.md "Run 2".
def ws( name, morph = 0 ):  vis.StartConfig( 'WaveShape', name, morph )
def cm( name, morph = 0 ):  vis.StartConfig( 'ColorMap', name, morph )
def ff( name, morph = 0 ):  vis.SetFlowField( name, '', morph, 0, True )
def shot( label ):
    print "reforce-probe shot: %s" % label
    vis.TakeScreenshot()

@_time( '0:00' )
def a000():
    vis.SetSlideshowEnabled( '*', False ); vis.KillRunningParticles( 0 ); vis.KillRunningSprites( 0 )
    vis.SetConsoleVisible( False )
    cm( 'zz reforce gray' ); ff( 'zz reforce void' ); ws( 'zz reforce hlines' )
@_time( '0:03' )
def a003(): shot( '01-hlines-void' )                       # 0001 line widths, no trails
@_time( '0:04' )
def a004(): ff( 'zz reforce rnd vert' )
@_time( '0:07' )
def a007(): shot( '02-rnd-vert' )                          # 0002
@_time( '0:08' )
def a008(): ff( 'zz reforce void' ); ws( 'zz reforce s3' )
@_time( '0:10' )
def a010(): shot( '03-s3' )                                # 0003
@_time( '0:11' )
def a011(): ws( 'zz reforce overlap' )
@_time( '0:13' )
def a013(): shot( '04-overlap' )                           # 0004
@_time( '0:14' )
def a014(): ws( 'zz reforce clock' )
@_time( '0:16' )
def a016(): shot( '05-clock-a' )                           # 0005
@_time( '0:18' )
def a018(): shot( '05-clock-b' )                           # 0006 (2 s later)
@_time( '0:19' )
def a019(): ws( 'zz reforce circle' )
@_time( '0:21' )
def a021(): ws( 'zz reforce two lines', 10 )               # 1 path -> 2 paths
@_time( '0:21.5' )
def a021b(): shot( '06-morph12-05pct' )                    # 0007
@_time( '0:22' )
def a022(): shot( '06-morph12-10pct' )                     # 0008
@_time( '0:23' )
def a023(): shot( '06-morph12-20pct' )                     # 0009
@_time( '0:26' )
def a026(): shot( '06-morph12-50pct' )                     # 0010
@_time( '0:33' )
def a033(): ws( 'zz reforce circle', 10 )                  # 2 paths -> 1 path
@_time( '0:33.5' )
def a033b(): shot( '07-morph21-05pct' )                    # 0011
@_time( '0:34' )
def a034(): shot( '07-morph21-10pct' )                     # 0012
@_time( '0:35' )
def a035(): shot( '07-morph21-20pct' )                     # 0013
@_time( '0:38' )
def a038(): shot( '07-morph21-50pct' )                     # 0014
@_time( '0:41' )
def a041(): shot( '07-morph21-80pct' )                     # 0015
@_time( '0:45' )
def a045(): print "reforce-probe: done"
