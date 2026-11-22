# reforce black-box probe run 40 (G-Force script, Python 2.7): particle slideshow spawn timing. Void field, gray map,
# WaveShape one dot (at -.5, 0). Only the Particle slideshow runs, with PROBE_PREFS Interval 5 s (deviation 0),
# Particle.Duration 2 s and Transition 0.5 s (deviations 0), so a particle without its own PDur lives 3 s: spawns every 5 s
# (interval from spawn to spawn), every 8 s (from the end of the last), or at random times. Shots every 0.5 s.
def ws( name, morph = 0 ):  vis.StartConfig( 'WaveShape', name, morph )
def cm( name, morph = 0 ):  vis.StartConfig( 'ColorMap', name, morph )
def ff( name, union = '', morph = 0 ):  vis.SetFlowField( name, union, morph, 0, True )
def shot( label ):
    print "reforce-probe shot: %s" % label
    vis.TakeScreenshot()
@_time( '0:00' )
def e000(): vis.SetSlideshowEnabled( '*', False ); vis.KillRunningParticles( 0 ); vis.KillRunningSprites( 0 ); vis.SetConsoleVisible( False ); cm( 'zz reforce gray' ); ff( 'zz reforce void' ); ws( 'zz reforce one dot' )
@_time( '0:01' )
def e001(): vis.SetSlideshowEnabled( 'Particle', True ); print 'reforce-probe: particle slideshow on'
@_time( '0:01.50' )
def e002(): shot( 'slideshow +0.5' )   # 0001
@_time( '0:02' )
def e003(): shot( 'slideshow +1.0' )   # 0002
@_time( '0:02.50' )
def e004(): shot( 'slideshow +1.5' )   # 0003
@_time( '0:03' )
def e005(): shot( 'slideshow +2.0' )   # 0004
@_time( '0:03.50' )
def e006(): shot( 'slideshow +2.5' )   # 0005
@_time( '0:04' )
def e007(): shot( 'slideshow +3.0' )   # 0006
@_time( '0:04.50' )
def e008(): shot( 'slideshow +3.5' )   # 0007
@_time( '0:05' )
def e009(): shot( 'slideshow +4.0' )   # 0008
@_time( '0:05.50' )
def e010(): shot( 'slideshow +4.5' )   # 0009
@_time( '0:06' )
def e011(): shot( 'slideshow +5.0' )   # 0010
@_time( '0:06.50' )
def e012(): shot( 'slideshow +5.5' )   # 0011
@_time( '0:07' )
def e013(): shot( 'slideshow +6.0' )   # 0012
@_time( '0:07.50' )
def e014(): shot( 'slideshow +6.5' )   # 0013
@_time( '0:08' )
def e015(): shot( 'slideshow +7.0' )   # 0014
@_time( '0:08.50' )
def e016(): shot( 'slideshow +7.5' )   # 0015
@_time( '0:09' )
def e017(): shot( 'slideshow +8.0' )   # 0016
@_time( '0:09.50' )
def e018(): shot( 'slideshow +8.5' )   # 0017
@_time( '0:10' )
def e019(): shot( 'slideshow +9.0' )   # 0018
@_time( '0:10.50' )
def e020(): shot( 'slideshow +9.5' )   # 0019
@_time( '0:11' )
def e021(): shot( 'slideshow +10.0' )   # 0020
@_time( '0:11.50' )
def e022(): shot( 'slideshow +10.5' )   # 0021
@_time( '0:12' )
def e023(): shot( 'slideshow +11.0' )   # 0022
@_time( '0:12.50' )
def e024(): shot( 'slideshow +11.5' )   # 0023
@_time( '0:13' )
def e025(): shot( 'slideshow +12.0' )   # 0024
@_time( '0:13.50' )
def e026(): shot( 'slideshow +12.5' )   # 0025
@_time( '0:14' )
def e027(): shot( 'slideshow +13.0' )   # 0026
@_time( '0:14.50' )
def e028(): shot( 'slideshow +13.5' )   # 0027
@_time( '0:15' )
def e029(): shot( 'slideshow +14.0' )   # 0028
@_time( '0:15.50' )
def e030(): shot( 'slideshow +14.5' )   # 0029
@_time( '0:16' )
def e031(): shot( 'slideshow +15.0' )   # 0030
@_time( '0:16.50' )
def e032(): shot( 'slideshow +15.5' )   # 0031
@_time( '0:17' )
def e033(): shot( 'slideshow +16.0' )   # 0032
@_time( '0:17.50' )
def e034(): shot( 'slideshow +16.5' )   # 0033
@_time( '0:18' )
def e035(): shot( 'slideshow +17.0' )   # 0034
@_time( '0:18.50' )
def e036(): shot( 'slideshow +17.5' )   # 0035
@_time( '0:19' )
def e037(): shot( 'slideshow +18.0' )   # 0036
@_time( '0:19.50' )
def e038(): shot( 'slideshow +18.5' )   # 0037
@_time( '0:20' )
def e039(): shot( 'slideshow +19.0' )   # 0038
@_time( '0:20.50' )
def e040(): shot( 'slideshow +19.5' )   # 0039
@_time( '0:21' )
def e041(): shot( 'slideshow +20.0' )   # 0040
@_time( '0:21.50' )
def e042(): shot( 'slideshow +20.5' )   # 0041
@_time( '0:22' )
def e043(): shot( 'slideshow +21.0' )   # 0042
@_time( '0:22.50' )
def e044(): shot( 'slideshow +21.5' )   # 0043
@_time( '0:23' )
def e045(): shot( 'slideshow +22.0' )   # 0044
@_time( '0:23.50' )
def e046(): shot( 'slideshow +22.5' )   # 0045
@_time( '0:24' )
def e047(): shot( 'slideshow +23.0' )   # 0046
@_time( '0:24.50' )
def e048(): shot( 'slideshow +23.5' )   # 0047
@_time( '0:25' )
def e049(): shot( 'slideshow +24.0' )   # 0048
@_time( '0:25.50' )
def e050(): shot( 'slideshow +24.5' )   # 0049
@_time( '0:26' )
def e051(): shot( 'slideshow +25.0' )   # 0050
@_time( '0:26.50' )
def e052(): shot( 'slideshow +25.5' )   # 0051
@_time( '0:27' )
def e053(): shot( 'slideshow +26.0' )   # 0052
@_time( '0:27.50' )
def e054(): shot( 'slideshow +26.5' )   # 0053
@_time( '0:28' )
def e055(): shot( 'slideshow +27.0' )   # 0054
@_time( '0:28.50' )
def e056(): shot( 'slideshow +27.5' )   # 0055
@_time( '0:29' )
def e057(): shot( 'slideshow +28.0' )   # 0056
@_time( '0:29.50' )
def e058(): shot( 'slideshow +28.5' )   # 0057
@_time( '0:30' )
def e059(): shot( 'slideshow +29.0' )   # 0058
@_time( '0:30.50' )
def e060(): shot( 'slideshow +29.5' )   # 0059
@_time( '0:31' )
def e061(): shot( 'slideshow +30.0' )   # 0060
@_time( '0:31.50' )
def e062(): shot( 'slideshow +30.5' )   # 0061
@_time( '0:32' )
def e063(): shot( 'slideshow +31.0' )   # 0062
@_time( '0:32.50' )
def e064(): shot( 'slideshow +31.5' )   # 0063
@_time( '0:33' )
def e065(): shot( 'slideshow +32.0' )   # 0064
@_time( '0:33.50' )
def e066(): shot( 'slideshow +32.5' )   # 0065
@_time( '0:34' )
def e067(): shot( 'slideshow +33.0' )   # 0066
@_time( '0:34.50' )
def e068(): shot( 'slideshow +33.5' )   # 0067
@_time( '0:35' )
def e069(): shot( 'slideshow +34.0' )   # 0068
@_time( '0:35.50' )
def e070(): shot( 'slideshow +34.5' )   # 0069
@_time( '0:36' )
def e071(): shot( 'slideshow +35.0' )   # 0070
@_time( '0:36.50' )
def e072(): shot( 'slideshow +35.5' )   # 0071
@_time( '0:37' )
def e073(): shot( 'slideshow +36.0' )   # 0072
@_time( '0:37.50' )
def e074(): shot( 'slideshow +36.5' )   # 0073
@_time( '0:38' )
def e075(): shot( 'slideshow +37.0' )   # 0074
@_time( '0:38.50' )
def e076(): shot( 'slideshow +37.5' )   # 0075
@_time( '0:39' )
def e077(): shot( 'slideshow +38.0' )   # 0076
@_time( '0:39.50' )
def e078(): shot( 'slideshow +38.5' )   # 0077
@_time( '0:40' )
def e079(): shot( 'slideshow +39.0' )   # 0078
@_time( '0:40.50' )
def e080(): shot( 'slideshow +39.5' )   # 0079
@_time( '0:41' )
def e081(): shot( 'slideshow +40.0' )   # 0080
@_time( '0:42' )
def e082(): vis.SetSlideshowEnabled( '*', False ); vis.KillRunningParticles( 0 ); print "reforce-probe: done"
