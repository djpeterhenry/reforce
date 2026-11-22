# reforce black-box probe run22 (G-Force script, Python 2.7): 5F.6 diagnostic. Console visible. External sprite
# folder holds our PNG test sprite (control) and our Card test movies; which names start?
def ws( name, morph = 0 ):  vis.StartConfig( 'WaveShape', name, morph )
def cm( name, morph = 0 ):  vis.StartConfig( 'ColorMap', name, morph )
def ff( name, union = '', morph = 0 ):  vis.SetFlowField( name, union, morph, 0, True )
def shot( label ):
    print "reforce-probe shot: %s" % label
    vis.TakeScreenshot()

@_time( '0:00' )
def a000():
    vis.SetSlideshowEnabled( '*', False ); vis.KillRunningParticles( 0 ); vis.KillRunningSprites( 0 )
    vis.SetConsoleVisible( True )
    cm( 'zz reforce gray' ); ff( 'zz reforce void' ); ws( 'zz reforce one dot' )
@_time( '0:01' )
def a001(): print "reforce: start png sprite"; vis.StartSprite( 'zz reforce sprite', 3 )
@_time( '0:03' )
def s001(): shot( '01-png' )        # 0001
@_time( '0:04' )
def a004(): vis.KillRunningSprites( 0 ); print "reforce: start movie a"; vis.StartSprite( 'zz reforce movie a', 3 )
@_time( '0:06' )
def s002(): shot( '02-movie-a' )        # 0002
@_time( '0:07' )
def a007(): vis.KillRunningSprites( 0 ); print "reforce: start movie a.mov"; vis.StartSprite( 'zz reforce movie a.mov', 3 )
@_time( '0:09' )
def s003(): shot( '03-movie-a.mov' )        # 0003
@_time( '0:10' )
def a010(): vis.KillRunningSprites( 0 ); print "reforce: start movie c (mp4)"; vis.StartSprite( 'zz reforce movie c', 3 )
@_time( '0:12' )
def s004(): shot( '04-movie-c' )        # 0004
@_time( '0:13' )
def a013(): vis.KillRunningSprites( 0 ); print "reforce: slideshow next sprite"; vis.SetSlideshowEnabled( 'Sprite', True )
@_time( '0:16' )
def s005(): shot( '05-slideshow' )        # 0005
@_time( '0:17' )
def done(): print "reforce-probe: done"
