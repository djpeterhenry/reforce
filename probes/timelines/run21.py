# reforce black-box probe run21 (G-Force script, Python 2.7): 5F.6 movie sprites. Our own Card test videos (reforce-gen-videos: frame index as 8 bit
# blocks on top) in an external sprite folder (PROBE_GLOBAL_PREFS G-Force.ExternalSprites.Path). Movie a: 640x360
# H.264 .mov, 30 fps, 150 frames (5 s), sprite duration 10 s (loops?). Movie b: 480x360 H.264 .mp4, 24 fps,
# 72 frames (3 s), 6 s. A clock WaveShape (path 0: t band) shows whether the sprite covers it.
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
    cm( 'zz reforce gray' ); ff( 'zz reforce void' ); ws( 'zz reforce morph strip' )
@_time( '0:02' )
def a002(): ws( 'zz reforce morph strip 2' ); vis.StartSprite( 'zz reforce movie a', 10 )
@_time( '0:02.25' )
def s001(): shot( '01-movie-a-t02.25' )        # 0001
@_time( '0:02.50' )
def s002(): shot( '02-movie-a-t02.50' )        # 0002
@_time( '0:02.75' )
def s003(): shot( '03-movie-a-t02.75' )        # 0003
@_time( '0:03.00' )
def s004(): shot( '04-movie-a-t03.00' )        # 0004
@_time( '0:03.25' )
def s005(): shot( '05-movie-a-t03.25' )        # 0005
@_time( '0:03.50' )
def s006(): shot( '06-movie-a-t03.50' )        # 0006
@_time( '0:03.75' )
def s007(): shot( '07-movie-a-t03.75' )        # 0007
@_time( '0:04.00' )
def s008(): shot( '08-movie-a-t04.00' )        # 0008
@_time( '0:04.25' )
def s009(): shot( '09-movie-a-t04.25' )        # 0009
@_time( '0:04.50' )
def s010(): shot( '10-movie-a-t04.50' )        # 0010
@_time( '0:04.75' )
def s011(): shot( '11-movie-a-t04.75' )        # 0011
@_time( '0:05.00' )
def s012(): shot( '12-movie-a-t05.00' )        # 0012
@_time( '0:05.25' )
def s013(): shot( '13-movie-a-t05.25' )        # 0013
@_time( '0:05.50' )
def s014(): shot( '14-movie-a-t05.50' )        # 0014
@_time( '0:05.75' )
def s015(): shot( '15-movie-a-t05.75' )        # 0015
@_time( '0:06.00' )
def s016(): shot( '16-movie-a-t06.00' )        # 0016
@_time( '0:06.25' )
def s017(): shot( '17-movie-a-t06.25' )        # 0017
@_time( '0:06.50' )
def s018(): shot( '18-movie-a-t06.50' )        # 0018
@_time( '0:06.75' )
def s019(): shot( '19-movie-a-t06.75' )        # 0019
@_time( '0:07.00' )
def s020(): shot( '20-movie-a-t07.00' )        # 0020
@_time( '0:07.25' )
def s021(): shot( '21-movie-a-t07.25' )        # 0021
@_time( '0:07.50' )
def s022(): shot( '22-movie-a-t07.50' )        # 0022
@_time( '0:07.75' )
def s023(): shot( '23-movie-a-t07.75' )        # 0023
@_time( '0:08.00' )
def s024(): shot( '24-movie-a-t08.00' )        # 0024
@_time( '0:08.25' )
def s025(): shot( '25-movie-a-t08.25' )        # 0025
@_time( '0:08.50' )
def s026(): shot( '26-movie-a-t08.50' )        # 0026
@_time( '0:08.75' )
def s027(): shot( '27-movie-a-t08.75' )        # 0027
@_time( '0:09.00' )
def s028(): shot( '28-movie-a-t09.00' )        # 0028
@_time( '0:09.25' )
def s029(): shot( '29-movie-a-t09.25' )        # 0029
@_time( '0:09.50' )
def s030(): shot( '30-movie-a-t09.50' )        # 0030
@_time( '0:09.75' )
def s031(): shot( '31-movie-a-t09.75' )        # 0031
@_time( '0:10.00' )
def s032(): shot( '32-movie-a-t10.00' )        # 0032
@_time( '0:10.25' )
def s033(): shot( '33-movie-a-t10.25' )        # 0033
@_time( '0:10.50' )
def s034(): shot( '34-movie-a-t10.50' )        # 0034
@_time( '0:10.75' )
def s035(): shot( '35-movie-a-t10.75' )        # 0035
@_time( '0:11.00' )
def s036(): shot( '36-movie-a-t11.00' )        # 0036
@_time( '0:11.25' )
def s037(): shot( '37-movie-a-t11.25' )        # 0037
@_time( '0:11.50' )
def s038(): shot( '38-movie-a-t11.50' )        # 0038
@_time( '0:11.75' )
def s039(): shot( '39-movie-a-t11.75' )        # 0039
@_time( '0:12.00' )
def s040(): shot( '40-movie-a-t12.00' )        # 0040
@_time( '0:12.25' )
def s041(): shot( '41-movie-a-t12.25' )        # 0041
@_time( '0:12.50' )
def s042(): shot( '42-movie-a-t12.50' )        # 0042
@_time( '0:12.75' )
def s043(): shot( '43-movie-a-t12.75' )        # 0043
@_time( '0:13.00' )
def s044(): shot( '44-movie-a-t13.00' )        # 0044
@_time( '0:13.25' )
def s045(): shot( '45-movie-a-t13.25' )        # 0045
@_time( '0:13.50' )
def s046(): shot( '46-movie-a-t13.50' )        # 0046
@_time( '0:13.75' )
def s047(): shot( '47-movie-a-t13.75' )        # 0047
@_time( '0:14.00' )
def s048(): shot( '48-movie-a-t14.00' )        # 0048
@_time( '0:14.25' )
def s049(): shot( '49-movie-a-t14.25' )        # 0049
@_time( '0:14.50' )
def s050(): shot( '50-movie-a-t14.50' )        # 0050
@_time( '0:14.75' )
def s051(): shot( '51-movie-a-t14.75' )        # 0051
@_time( '0:15.00' )
def s052(): shot( '52-movie-a-t15.00' )        # 0052
@_time( '0:15.25' )
def s053(): shot( '53-movie-a-t15.25' )        # 0053
@_time( '0:15.50' )
def s054(): shot( '54-movie-a-t15.50' )        # 0054
@_time( '0:15.75' )
def s055(): shot( '55-movie-a-t15.75' )        # 0055
@_time( '0:16.00' )
def s056(): shot( '56-movie-a-t16.00' )        # 0056
@_time( '0:16.25' )
def s057(): shot( '57-movie-a-t16.25' )        # 0057
@_time( '0:16.50' )
def s058(): shot( '58-movie-a-t16.50' )        # 0058
@_time( '0:16.75' )
def s059(): shot( '59-movie-a-t16.75' )        # 0059
@_time( '0:17.00' )
def s060(): shot( '60-movie-a-t17.00' )        # 0060
@_time( '0:17.25' )
def s061(): shot( '61-movie-a-t17.25' )        # 0061
@_time( '0:17.50' )
def s062(): shot( '62-movie-a-t17.50' )        # 0062
@_time( '0:17.75' )
def s063(): shot( '63-movie-a-t17.75' )        # 0063
@_time( '0:18.00' )
def s064(): shot( '64-movie-a-t18.00' )        # 0064
@_time( '0:18.25' )
def s065(): shot( '65-movie-a-t18.25' )        # 0065
@_time( '0:18.50' )
def s066(): shot( '66-movie-a-t18.50' )        # 0066
@_time( '0:18.75' )
def s067(): shot( '67-movie-a-t18.75' )        # 0067
@_time( '0:19.00' )
def s068(): shot( '68-movie-a-t19.00' )        # 0068
@_time( '0:19.25' )
def s069(): shot( '69-movie-a-t19.25' )        # 0069
@_time( '0:19.50' )
def s070(): shot( '70-movie-a-t19.50' )        # 0070
@_time( '0:19.75' )
def s071(): shot( '71-movie-a-t19.75' )        # 0071
@_time( '0:20.00' )
def s072(): shot( '72-movie-a-t20.00' )        # 0072
@_time( '0:20.25' )
def s073(): shot( '73-movie-a-t20.25' )        # 0073
@_time( '0:20.50' )
def s074(): shot( '74-movie-a-t20.50' )        # 0074
@_time( '0:20.75' )
def s075(): shot( '75-movie-a-t20.75' )        # 0075
@_time( '0:21.00' )
def s076(): shot( '76-movie-a-t21.00' )        # 0076
@_time( '0:21.25' )
def s077(): shot( '77-movie-a-t21.25' )        # 0077
@_time( '0:21.50' )
def s078(): shot( '78-movie-a-t21.50' )        # 0078
@_time( '0:21.75' )
def s079(): shot( '79-movie-a-t21.75' )        # 0079
@_time( '0:22.00' )
def s080(): shot( '80-movie-a-t22.00' )        # 0080
@_time( '0:22.25' )
def s081(): shot( '81-movie-a-t22.25' )        # 0081
@_time( '0:22.50' )
def s082(): shot( '82-movie-a-t22.50' )        # 0082
@_time( '0:22.75' )
def s083(): shot( '83-movie-a-t22.75' )        # 0083
@_time( '0:23.00' )
def s084(): shot( '84-movie-a-t23.00' )        # 0084
@_time( '0:23.25' )
def s085(): shot( '85-movie-a-t23.25' )        # 0085
@_time( '0:23.50' )
def s086(): shot( '86-movie-a-t23.50' )        # 0086
@_time( '0:23.75' )
def s087(): shot( '87-movie-a-t23.75' )        # 0087
@_time( '0:24.00' )
def s088(): shot( '88-movie-a-t24.00' )        # 0088
@_time( '0:24.25' )
def s089(): shot( '89-movie-a-t24.25' )        # 0089
@_time( '0:24.50' )
def s090(): shot( '90-movie-a-t24.50' )        # 0090
@_time( '0:24.75' )
def s091(): shot( '91-movie-a-t24.75' )        # 0091
@_time( '0:25.00' )
def s092(): shot( '92-movie-a-t25.00' )        # 0092
@_time( '0:26' )
def a026(): vis.KillRunningSprites( 0 ); ws( 'zz reforce morph strip' ); vis.StartSprite( 'zz reforce movie b', 6 )
@_time( '0:26.25' )
def s093(): shot( '93-movie-b-t26.25' )        # 0093
@_time( '0:26.50' )
def s094(): shot( '94-movie-b-t26.50' )        # 0094
@_time( '0:26.75' )
def s095(): shot( '95-movie-b-t26.75' )        # 0095
@_time( '0:27.00' )
def s096(): shot( '96-movie-b-t27.00' )        # 0096
@_time( '0:27.25' )
def s097(): shot( '97-movie-b-t27.25' )        # 0097
@_time( '0:27.50' )
def s098(): shot( '98-movie-b-t27.50' )        # 0098
@_time( '0:27.75' )
def s099(): shot( '99-movie-b-t27.75' )        # 0099
@_time( '0:28.00' )
def s100(): shot( '100-movie-b-t28.00' )        # 0100
@_time( '0:28.25' )
def s101(): shot( '101-movie-b-t28.25' )        # 0101
@_time( '0:28.50' )
def s102(): shot( '102-movie-b-t28.50' )        # 0102
@_time( '0:28.75' )
def s103(): shot( '103-movie-b-t28.75' )        # 0103
@_time( '0:29.00' )
def s104(): shot( '104-movie-b-t29.00' )        # 0104
@_time( '0:29.25' )
def s105(): shot( '105-movie-b-t29.25' )        # 0105
@_time( '0:29.50' )
def s106(): shot( '106-movie-b-t29.50' )        # 0106
@_time( '0:29.75' )
def s107(): shot( '107-movie-b-t29.75' )        # 0107
@_time( '0:30.00' )
def s108(): shot( '108-movie-b-t30.00' )        # 0108
@_time( '0:30.25' )
def s109(): shot( '109-movie-b-t30.25' )        # 0109
@_time( '0:30.50' )
def s110(): shot( '110-movie-b-t30.50' )        # 0110
@_time( '0:30.75' )
def s111(): shot( '111-movie-b-t30.75' )        # 0111
@_time( '0:31.00' )
def s112(): shot( '112-movie-b-t31.00' )        # 0112
@_time( '0:31.25' )
def s113(): shot( '113-movie-b-t31.25' )        # 0113
@_time( '0:31.50' )
def s114(): shot( '114-movie-b-t31.50' )        # 0114
@_time( '0:31.75' )
def s115(): shot( '115-movie-b-t31.75' )        # 0115
@_time( '0:32.00' )
def s116(): shot( '116-movie-b-t32.00' )        # 0116
@_time( '0:32.25' )
def s117(): shot( '117-movie-b-t32.25' )        # 0117
@_time( '0:32.50' )
def s118(): shot( '118-movie-b-t32.50' )        # 0118
@_time( '0:32.75' )
def s119(): shot( '119-movie-b-t32.75' )        # 0119
@_time( '0:33.00' )
def s120(): shot( '120-movie-b-t33.00' )        # 0120
@_time( '0:33.25' )
def s121(): shot( '121-movie-b-t33.25' )        # 0121
@_time( '0:33.50' )
def s122(): shot( '122-movie-b-t33.50' )        # 0122
@_time( '0:33.75' )
def s123(): shot( '123-movie-b-t33.75' )        # 0123
@_time( '0:34.00' )
def s124(): shot( '124-movie-b-t34.00' )        # 0124
@_time( '0:34.25' )
def s125(): shot( '125-movie-b-t34.25' )        # 0125
@_time( '0:34.50' )
def s126(): shot( '126-movie-b-t34.50' )        # 0126
@_time( '0:34.75' )
def s127(): shot( '127-movie-b-t34.75' )        # 0127
@_time( '0:35.00' )
def s128(): shot( '128-movie-b-t35.00' )        # 0128
@_time( '0:35.25' )
def s129(): shot( '129-movie-b-t35.25' )        # 0129
@_time( '0:35.50' )
def s130(): shot( '130-movie-b-t35.50' )        # 0130
@_time( '0:35.75' )
def s131(): shot( '131-movie-b-t35.75' )        # 0131
@_time( '0:36.00' )
def s132(): shot( '132-movie-b-t36.00' )        # 0132
@_time( '0:36.25' )
def s133(): shot( '133-movie-b-t36.25' )        # 0133
@_time( '0:36.50' )
def s134(): shot( '134-movie-b-t36.50' )        # 0134
@_time( '0:36.75' )
def s135(): shot( '135-movie-b-t36.75' )        # 0135
@_time( '0:37.00' )
def s136(): shot( '136-movie-b-t37.00' )        # 0136
@_time( '0:37.25' )
def s137(): shot( '137-movie-b-t37.25' )        # 0137
@_time( '0:37.50' )
def s138(): shot( '138-movie-b-t37.50' )        # 0138
@_time( '0:37.75' )
def s139(): shot( '139-movie-b-t37.75' )        # 0139
@_time( '0:38.00' )
def s140(): shot( '140-movie-b-t38.00' )        # 0140
@_time( '0:38.25' )
def s141(): shot( '141-movie-b-t38.25' )        # 0141
@_time( '0:38.50' )
def s142(): shot( '142-movie-b-t38.50' )        # 0142
@_time( '0:38.75' )
def s143(): shot( '143-movie-b-t38.75' )        # 0143
@_time( '0:39.00' )
def s144(): shot( '144-movie-b-t39.00' )        # 0144
@_time( '0:39.25' )
def s145(): shot( '145-movie-b-t39.25' )        # 0145
@_time( '0:39.50' )
def s146(): shot( '146-movie-b-t39.50' )        # 0146
@_time( '0:39.75' )
def s147(): shot( '147-movie-b-t39.75' )        # 0147
@_time( '0:40.00' )
def s148(): shot( '148-movie-b-t40.00' )        # 0148
@_time( '0:40.25' )
def s149(): shot( '149-movie-b-t40.25' )        # 0149
@_time( '0:40.50' )
def s150(): shot( '150-movie-b-t40.50' )        # 0150
@_time( '0:40.75' )
def s151(): shot( '151-movie-b-t40.75' )        # 0151
@_time( '0:41.00' )
def s152(): shot( '152-movie-b-t41.00' )        # 0152
@_time( '0:41.25' )
def s153(): shot( '153-movie-b-t41.25' )        # 0153
@_time( '0:41.50' )
def s154(): shot( '154-movie-b-t41.50' )        # 0154
@_time( '0:41.75' )
def s155(): shot( '155-movie-b-t41.75' )        # 0155
@_time( '0:42.00' )
def s156(): shot( '156-movie-b-t42.00' )        # 0156
@_time( '0:43' )
def done(): print "reforce-probe: done"
