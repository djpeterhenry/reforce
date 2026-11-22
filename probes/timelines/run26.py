# reforce black-box probe run 26 (G-Force script, Python 2.7): how sprites layer with the FlowField and WaveShape.
# Sprites from build/probe-sprites26 (tools/parity/make_sprite26.py) via G-Force.ExternalSprites.Path; run with
# PROBE_PREFS Sprite.Transition.Duration=2, deviation 0 (fade in 2 s, hold = StartSprite's 4 s, fade out 2 s).
# The grid WaveShape is redrawn every frame (so decay doesn't confound, unlike run 25).
#  1. identity field: is the grid drawn over the sprite or hidden under it? Crossfade or per-frame stamp? Does
#     anything of the sprite remain after it ends?
#  2. zoom-in field (content shrinks toward the centre): does the sprite stream inward with the flow (stamped into
#     the feedback) or stay a clean layer?
#  3. Pen / Pen+EPen / EPen-only (sibling .txt, X=.2 marks that it was read), full envelope then an instant kill:
#     which part is displayed, which part feeds back (trail after the kill)?
def ws( name, morph = 0 ):  vis.StartConfig( 'WaveShape', name, morph )
def cm( name, morph = 0 ):  vis.StartConfig( 'ColorMap', name, morph )
def ff( name, union = '', morph = 0 ):  vis.SetFlowField( name, union, morph, 0, True )
def shot( label ):
    print "reforce-probe shot: %s" % label
    vis.TakeScreenshot()
def sprite( name, dur ):
    try:
        vis.StartSprite( name, dur )
        print "reforce-probe: StartSprite(%r, %r) ok" % ( name, dur )
    except Exception, e:
        print "reforce-probe: StartSprite(%r) failed: %s" % ( name, e )

@_time( '0:00' )
def a000():
    vis.SetSlideshowEnabled( '*', False ); vis.KillRunningParticles( 0 ); vis.KillRunningSprites( 0 )
    vis.SetConsoleVisible( False )
    cm( 'zz reforce gray' ); ff( 'zz reforce identity' ); ws( 'zz reforce grid' )

# 1. identity field, grid redrawn
@_time( '0:02' )
def a020(): shot( '01-grid' )                    # 0001
@_time( '0:02.5' )
def a025(): sprite( 'zz reforce sq', 4 )         # in 2.5-4.5, full 4.5-8.5, out 8.5-10.5
@_time( '0:03.5' )
def a035(): shot( '02-id-fadein-1s' )            # 0002
@_time( '0:06' )
def a060(): shot( '03-id-full' )                 # 0003
@_time( '0:09.5' )
def a095(): shot( '04-id-fadeout-1s' )           # 0004
@_time( '0:11' )
def a110(): shot( '05-id-after' )                # 0005

# 2. zoom-in field, grid redrawn
@_time( '0:12' )
def a120(): ff( 'zz reforce zoom in' )
@_time( '0:13' )
def a130(): sprite( 'zz reforce sq', 4 )         # in 13-15, full 15-19, out 19-21
@_time( '0:14' )
def a140(): shot( '06-zoom-fadein-1s' )          # 0006
@_time( '0:17' )
def a170(): shot( '07-zoom-full' )               # 0007
@_time( '0:20' )
def a200(): shot( '08-zoom-fadeout-1s' )         # 0008
@_time( '0:21.5' )
def a215(): shot( '09-zoom-after' )              # 0009

# 3. Pen / EPen variants on the zoom field: full envelope, then an instant kill
@_time( '0:23' )
def a230(): sprite( 'zz reforce sq pen', 10 )
@_time( '0:26' )
def a260(): shot( '10-pen-full' )                # 0010
@_time( '0:27' )
def a270(): vis.KillRunningSprites( 0 )
@_time( '0:27.3' )
def a273(): shot( '11-pen-killed' )              # 0011
@_time( '0:29' )
def a290(): sprite( 'zz reforce sq both', 10 )
@_time( '0:32' )
def a320(): shot( '12-both-full' )               # 0012
@_time( '0:33' )
def a330(): vis.KillRunningSprites( 0 )
@_time( '0:33.3' )
def a333(): shot( '13-both-killed' )             # 0013
@_time( '0:35' )
def a350(): sprite( 'zz reforce sq epen', 10 )
@_time( '0:38' )
def a380(): shot( '14-epen-full' )               # 0014
@_time( '0:39' )
def a390(): vis.KillRunningSprites( 0 )
@_time( '0:39.3' )
def a393(): shot( '15-epen-killed' )             # 0015

# 4. same, identity field (pure display vs feedback without motion)
@_time( '0:40' )
def a400(): ff( 'zz reforce identity' )
@_time( '0:41' )
def a410(): sprite( 'zz reforce sq pen', 10 )
@_time( '0:44' )
def a440(): shot( '16-id-pen-full' )             # 0016
@_time( '0:45' )
def a450(): vis.KillRunningSprites( 0 )
@_time( '0:45.3' )
def a453(): shot( '17-id-pen-killed' )           # 0017
@_time( '0:46' )
def a460(): sprite( 'zz reforce sq epen', 10 )
@_time( '0:49' )
def a490(): shot( '18-id-epen-full' )            # 0018
@_time( '0:50' )
def a500(): vis.KillRunningSprites( 0 )
@_time( '0:50.3' )
def a503(): shot( '19-id-epen-killed' )          # 0019
@_time( '0:51' )
def a510(): print "reforce-probe: done"
