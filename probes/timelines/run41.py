# reforce black-box probe run 41 (G-Force script, Python 2.7): the end of a FlowField Union transition (#18 left the
# end-of-transition jump unmeasured; run 5's attempt saw no grid). A circle (r .5) redrawn every frame on identity ->
# zoom in (source 1.03 p) leaves one ring per frame that moves inward: a ring's brightness = 255 - its age in frames,
# its radius = the product of the per-frame zooms since it was drawn. With 'zz reforce union const' (ALFA = .5) the
# per-frame zoom is 1 + .03 w ALFA during the 4 s morph: does it jump from 1.015 to 1.03 at the end? Control: the
# same morph without a Union. Void field between cases clears the trails.
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
@_time( '0:05' )
def e002(): shot( 'union 3.0 s' )   # 0001
@_time( '0:06.30' )
def e003(): shot( 'union end +0.3' )   # 0002
@_time( '0:07' )
def e004(): shot( 'union end +1.0' )   # 0003
@_time( '0:08' )
def e005(): ff( 'zz reforce void' )
@_time( '0:08.50' )
def e006(): ff( 'zz reforce identity' )
@_time( '0:10' )
def e007(): ff( 'zz reforce zoom in', '', 4 )
@_time( '0:13' )
def e008(): shot( 'plain 3.0 s' )   # 0004
@_time( '0:14.30' )
def e009(): shot( 'plain end +0.3' )   # 0005
@_time( '0:15' )
def e010(): shot( 'plain end +1.0' )   # 0006
@_time( '0:16' )
def e011(): print "reforce-probe: done"
