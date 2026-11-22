# reforce black-box probe run17 (G-Force script, Python 2.7): 5B.1 FlowField morph timing. Three 10 s morphs a->b->a->b, each started in the same call as a fresh clock
# WaveShape (B0 = frames since the call). The morph dot's trail (strip probe) records the weight w per frame.
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
@_time( '0:03' )
def m0(): ws( 'zz reforce morph strip 2' ); ff( 'zz reforce morph b', '', 10 )
@_time( '0:03.50' )
def s001(): shot( '01-morph0-t03.50' )        # 0001
@_time( '0:05.00' )
def s002(): shot( '02-morph0-t05.00' )        # 0002
@_time( '0:06.50' )
def s003(): shot( '03-morph0-t06.50' )        # 0003
@_time( '0:08.00' )
def s004(): shot( '04-morph0-t08.00' )        # 0004
@_time( '0:09.50' )
def s005(): shot( '05-morph0-t09.50' )        # 0005
@_time( '0:11.00' )
def s006(): shot( '06-morph0-t11.00' )        # 0006
@_time( '0:12.50' )
def s007(): shot( '07-morph0-t12.50' )        # 0007
@_time( '0:14.00' )
def s008(): shot( '08-morph0-t14.00' )        # 0008
@_time( '0:17' )
def m1(): ws( 'zz reforce morph strip' ); ff( 'zz reforce morph a', '', 10 )
@_time( '0:17.50' )
def s009(): shot( '09-morph1-t17.50' )        # 0009
@_time( '0:19.00' )
def s010(): shot( '10-morph1-t19.00' )        # 0010
@_time( '0:20.50' )
def s011(): shot( '11-morph1-t20.50' )        # 0011
@_time( '0:22.00' )
def s012(): shot( '12-morph1-t22.00' )        # 0012
@_time( '0:23.50' )
def s013(): shot( '13-morph1-t23.50' )        # 0013
@_time( '0:25.00' )
def s014(): shot( '14-morph1-t25.00' )        # 0014
@_time( '0:26.50' )
def s015(): shot( '15-morph1-t26.50' )        # 0015
@_time( '0:28.00' )
def s016(): shot( '16-morph1-t28.00' )        # 0016
@_time( '0:31' )
def m2(): ws( 'zz reforce morph strip 2' ); ff( 'zz reforce morph b', '', 10 )
@_time( '0:31.50' )
def s017(): shot( '17-morph2-t31.50' )        # 0017
@_time( '0:33.00' )
def s018(): shot( '18-morph2-t33.00' )        # 0018
@_time( '0:34.50' )
def s019(): shot( '19-morph2-t34.50' )        # 0019
@_time( '0:36.00' )
def s020(): shot( '20-morph2-t36.00' )        # 0020
@_time( '0:37.50' )
def s021(): shot( '21-morph2-t37.50' )        # 0021
@_time( '0:39.00' )
def s022(): shot( '22-morph2-t39.00' )        # 0022
@_time( '0:40.50' )
def s023(): shot( '23-morph2-t40.50' )        # 0023
@_time( '0:42.00' )
def s024(): shot( '24-morph2-t42.00' )        # 0024
@_time( '0:46' )
def done(): print "reforce-probe: done"
