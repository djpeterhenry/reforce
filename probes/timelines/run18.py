# reforce black-box probe run18 (G-Force script, Python 2.7): 5B.2 particle fade envelope, dense. Three particles (PDur 2) started 1 s apart on the
# scrolling strip; column age + pixel value = envelope x 255 per frame. Shots every 0.5 s.
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
    cm( 'zz reforce gray' ); ff( 'zz reforce morph a' ); ws( 'zz reforce morph strip' )
@_time( '0:02' )
def p0(): ws( 'zz reforce morph strip 2' ); vis.StartParticle( 'zz reforce pstrip 0', 2 )
@_time( '0:03' )
def p1(): vis.StartParticle( 'zz reforce pstrip 1', 2 )
@_time( '0:04' )
def p2(): vis.StartParticle( 'zz reforce pstrip 2', 2 )
@_time( '0:02.50' )
def s001(): shot( '01-particles-t02.50' )        # 0001
@_time( '0:03.00' )
def s002(): shot( '02-particles-t03.00' )        # 0002
@_time( '0:03.50' )
def s003(): shot( '03-particles-t03.50' )        # 0003
@_time( '0:04.00' )
def s004(): shot( '04-particles-t04.00' )        # 0004
@_time( '0:04.50' )
def s005(): shot( '05-particles-t04.50' )        # 0005
@_time( '0:05.00' )
def s006(): shot( '06-particles-t05.00' )        # 0006
@_time( '0:05.50' )
def s007(): shot( '07-particles-t05.50' )        # 0007
@_time( '0:06.00' )
def s008(): shot( '08-particles-t06.00' )        # 0008
@_time( '0:06.50' )
def s009(): shot( '09-particles-t06.50' )        # 0009
@_time( '0:07.00' )
def s010(): shot( '10-particles-t07.00' )        # 0010
@_time( '0:07.50' )
def s011(): shot( '11-particles-t07.50' )        # 0011
@_time( '0:08.00' )
def s012(): shot( '12-particles-t08.00' )        # 0012
@_time( '0:08.50' )
def s013(): shot( '13-particles-t08.50' )        # 0013
@_time( '0:09.00' )
def s014(): shot( '14-particles-t09.00' )        # 0014
@_time( '0:09.50' )
def s015(): shot( '15-particles-t09.50' )        # 0015
@_time( '0:10.00' )
def s016(): shot( '16-particles-t10.00' )        # 0016
@_time( '0:10.50' )
def s017(): shot( '17-particles-t10.50' )        # 0017
@_time( '0:11.00' )
def s018(): shot( '18-particles-t11.00' )        # 0018
@_time( '0:11.50' )
def s019(): shot( '19-particles-t11.50' )        # 0019
@_time( '0:12.00' )
def s020(): shot( '20-particles-t12.00' )        # 0020
@_time( '0:12.50' )
def s021(): shot( '21-particles-t12.50' )        # 0021
@_time( '0:13.00' )
def s022(): shot( '22-particles-t13.00' )        # 0022
@_time( '0:13.50' )
def s023(): shot( '23-particles-t13.50' )        # 0023
@_time( '0:14.00' )
def s024(): shot( '24-particles-t14.00' )        # 0024
@_time( '0:14.50' )
def s025(): shot( '25-particles-t14.50' )        # 0025
@_time( '0:15.00' )
def s026(): shot( '26-particles-t15.00' )        # 0026
@_time( '0:15.50' )
def s027(): shot( '27-particles-t15.50' )        # 0027
@_time( '0:16.00' )
def s028(): shot( '28-particles-t16.00' )        # 0028
@_time( '0:16.50' )
def s029(): shot( '29-particles-t16.50' )        # 0029
@_time( '0:17.00' )
def s030(): shot( '30-particles-t17.00' )        # 0030
@_time( '0:17.50' )
def s031(): shot( '31-particles-t17.50' )        # 0031
@_time( '0:18.00' )
def s032(): shot( '32-particles-t18.00' )        # 0032
@_time( '0:18.50' )
def s033(): shot( '33-particles-t18.50' )        # 0033
@_time( '0:19.00' )
def s034(): shot( '34-particles-t19.00' )        # 0034
@_time( '0:19.50' )
def s035(): shot( '35-particles-t19.50' )        # 0035
@_time( '0:20' )
def done(): print "reforce-probe: done"
