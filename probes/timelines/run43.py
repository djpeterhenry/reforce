# reforce black-box probe run 43 (G-Force script, Python 2.7): what replaces a persisting Union blend (run 42: after
# identity -> zoom in with ALFA .5 the field stays at the ALFA-weighted zoom). Circle trail; after the union morph a
# plain 4 s morph to 'zz reforce zoom out' (misnamed: its source is 1.5 p, a strong zoom in, so the trail is rings
# 1.5x apart). At the end, the new field everywhere gives ring ratios of 1.5 like the control; a remaining blend less.
def ws( name, morph = 0 ):  vis.StartConfig( 'WaveShape', name, morph )
def cm( name, morph = 0 ):  vis.StartConfig( 'ColorMap', name, morph )
def ff( name, union = '', morph = 0 ):  vis.SetFlowField( name, union, morph, 0, True )
def shot( label ):
    print "reforce-probe shot: %s" % label
    vis.TakeScreenshot()
@_time( '0:00' )
def e000(): vis.SetSlideshowEnabled( '*', False ); vis.KillRunningParticles( 0 ); vis.KillRunningSprites( 0 ); vis.SetConsoleVisible( False ); cm( 'zz reforce gray' ); ff( 'zz reforce identity' ); ws( 'zz reforce circle' )
@_time( '0:02' )
def e001(): ff( 'zz reforce zoom in', 'zz reforce union const', 4 )
@_time( '0:09' )
def e002(): shot( 'union blend held' )   # 0001
@_time( '0:10' )
def e003(): ff( 'zz reforce zoom out', '', 4 )
@_time( '0:12' )
def e004(): shot( 'to zoom out +2' )   # 0002
@_time( '0:14.50' )
def e005(): shot( 'to zoom out +4.5' )   # 0003
@_time( '0:16' )
def e006(): shot( 'to zoom out +6' )   # 0004
@_time( '0:19' )
def e007(): shot( 'to zoom out +9' )   # 0005
@_time( '0:20' )
def e008(): ff( 'zz reforce identity' ); ws( 'zz reforce one dot' )
@_time( '0:21' )
def e009(): ff( 'zz reforce void' )
@_time( '0:21.50' )
def e010(): ff( 'zz reforce identity' ); ws( 'zz reforce circle' )
@_time( '0:23' )
def e011(): ff( 'zz reforce zoom out', '', 4 )
@_time( '0:29' )
def e012(): shot( 'control: plain zoom out +6' )   # 0006
@_time( '0:30' )
def e013(): print "reforce-probe: done"
