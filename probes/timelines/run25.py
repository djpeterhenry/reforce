# reforce black-box probe run 25 (G-Force script, Python 2.7): does the Space/Moon sprite (JPEG, moon on a black
# square; Moon.txt has EPen=1, no Pen, no Flag) wipe what is under its black corners, or is black transparent?
# A grid is drawn and then held by the identity field; the sprite fades in over it.
def ws( name, morph = 0 ):  vis.StartConfig( 'WaveShape', name, morph )
def cm( name, morph = 0 ):  vis.StartConfig( 'ColorMap', name, morph )
def ff( name, union = '', morph = 0 ):  vis.SetFlowField( name, union, morph, 0, True )
def shot( label ):
    print "reforce-probe shot: %s" % label
    vis.TakeScreenshot()
def moon():
    for args in ( ( 'Moon', 0 ), ( 'Moon', ), ( 'Space/Moon', 0 ) ):
        try:
            vis.StartSprite( *args )
            print "reforce-probe: StartSprite%r ok" % ( args, )
            return
        except Exception, e:
            print "reforce-probe: StartSprite%r failed: %s" % ( args, e )

@_time( '0:00' )
def a000():
    vis.SetSlideshowEnabled( '*', False ); vis.KillRunningParticles( 0 ); vis.KillRunningSprites( 0 )
    vis.SetConsoleVisible( False )
    cm( 'zz reforce gray' ); ff( 'zz reforce identity' ); ws( 'zz reforce grid' )
@_time( '0:03' )
def a003(): shot( '01-grid' )                              # 0001 the held grid alone
@_time( '0:04' )
def a004(): ws( 'zz reforce one dot' ); moon()
@_time( '0:07' )
def a007(): shot( '02-moon-3s' )                           # 0002
@_time( '0:11' )
def a011(): shot( '02-moon-7s' )                           # 0003
@_time( '0:15' )
def a015(): shot( '02-moon-11s' )                          # 0004
@_time( '0:16' )
def a016(): vis.KillRunningSprites( 0 ); print "reforce-probe: done"
