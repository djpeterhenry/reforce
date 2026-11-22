# reforce black-box probe run (G-Force script, Python 2.7).
# Uses only documented script commands. Screenshots go to Screenshot.Dir (set by probes/run.sh).
# Timeline: each section sets a known state, waits, then takes screenshots. See probes/README.md.

def ws( name, morph = 0 ):  vis.StartConfig( 'WaveShape', name, morph )
def cm( name, morph = 0 ):  vis.StartConfig( 'ColorMap', name, morph )
def ff( name, morph = 0 ):  vis.SetFlowField( name, '', morph, 0, True )
def shot( label ):
    print "reforce-probe shot: %s" % label
    vis.TakeScreenshot()

@_time( '0:00' )
def a000():
    print "reforce-probe: start"
    vis.SetSlideshowEnabled( '*', False )
    vis.KillRunningParticles( 0 )
    vis.KillRunningSprites( 0 )
    vis.SetConsoleVisible( False )
    cm( 'zz reforce gray' ); ff( 'zz reforce identity' ); ws( 'zz reforce circle' )

# 1. Decay: flash a bar under the identity field, then watch it fade.
@_time( '0:04' )
def a004(): ws( 'zz reforce flash' )
@_time( '0:04.25' )
def a004b(): shot( '01-decay-0.25s' )
@_time( '0:05' )
def a005(): shot( '01-decay-1.0s' )
@_time( '0:06' )
def a006(): shot( '01-decay-2.0s' )
@_time( '0:08' )
def a008(): shot( '01-decay-4.0s' )

# 2. Out-of-frame policy: zoom-out field with a circle.
@_time( '0:10' )
def a010(): ff( 'zz reforce zoom out' ); ws( 'zz reforce circle' )
@_time( '0:13' )
def a013(): shot( '02-zoom-out' )

# 3. Operator precedence dots.
@_time( '0:14' )
def a014(): ff( 'zz reforce identity' ); ws( 'zz reforce precedence' )
@_time( '0:15.5' )
def a015(): shot( '03-precedence' )

# 4. s spacing.
@_time( '0:17' )
def a017(): ws( 'zz reforce s spacing' )
@_time( '0:18.5' )
def a018(): shot( '04-s-spacing' )

# 5. B persistence and C carry-over.
@_time( '0:20' )
def a020(): ws( 'zz reforce b persistence' )
@_time( '0:20.2' )
def a020b(): shot( '05-persist-0.2s' )
@_time( '0:22' )
def a022(): shot( '05-persist-2.0s' )

# 6. rnd per row vs per pixel.
@_time( '0:24' )
def a024(): ff( 'zz reforce rnd pixel' ); ws( 'zz reforce two lines' )
@_time( '0:27' )
def a027(): shot( '06-rnd' )

# 7. ColorMap clamp + Pen ramp.
@_time( '0:28' )
def a028(): ff( 'zz reforce identity' ); ws( 'zz reforce ramp' ); cm( 'zz reforce gray' )
@_time( '0:30' )
def a030(): shot( '07-ramp-gray' )
@_time( '0:30.5' )
def a030b(): cm( 'zz reforce clamp' )
@_time( '0:32' )
def a032(): shot( '07-ramp-clamp' )

# 8. ColorMap morph red -> green over 10 s (ramp still drawing).
@_time( '0:33' )
def a033(): cm( 'zz reforce red' )
@_time( '0:35' )
def a035(): cm( 'zz reforce green', 10 )
@_time( '0:37.5' )
def a037(): shot( '08-cmap-morph-25pct' )
@_time( '0:40' )
def a040(): shot( '08-cmap-morph-50pct' )
@_time( '0:46' )
def a046(): shot( '08-cmap-morph-done' )

# 9. FlowField morph +rotation -> -rotation over 10 s (two lines drawing).
@_time( '0:47' )
def a047(): cm( 'zz reforce gray' ); ff( 'zz reforce rotate plus' ); ws( 'zz reforce two lines' )
@_time( '0:50' )
def a050(): shot( '09-ff-rot-plus' ); ff( 'zz reforce rotate minus', 10 )
@_time( '0:55' )
def a055(): shot( '09-ff-morph-50pct' )
@_time( '1:01' )
def a061(): shot( '09-ff-rot-minus' )

# 10. WaveShape morph circle -> two lines over 10 s, identity field.
@_time( '1:02' )
def a062(): ff( 'zz reforce identity' ); ws( 'zz reforce circle' )
@_time( '1:05' )
def a065(): ws( 'zz reforce two lines', 10 )
@_time( '1:07.5' )
def a067(): shot( '10-ws-morph-25pct' )
@_time( '1:10' )
def a070(): shot( '10-ws-morph-50pct' )
@_time( '1:12.5' )
def a072(): shot( '10-ws-morph-75pct' )

@_time( '1:17' )
def a077(): print "reforce-probe: done"
