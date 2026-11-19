/*
q3_rename.h -- gives every function and global that ioq3's q_shared.c and
q_math.c define a Q3_ prefix at link time, so the Quake III code (botlib and
the bot AI, src/baseq2/q3bot) keeps its own versions without clashing with
src/shared/shared.c and the game's own (va, Info_ValueForKey, vectoangles,
AngleVectors, vec3_origin...). The Quake III sources still use the plain
names; this header is included at the top of qcommon/q_shared.h.
*/

#ifndef Q3_RENAME_H
#define Q3_RENAME_H

#define _DotProduct                  Q3__DotProduct
#define _VectorAdd                   Q3__VectorAdd
#define _VectorCopy                  Q3__VectorCopy
#define _VectorMA                    Q3__VectorMA
#define _VectorScale                 Q3__VectorScale
#define _VectorSubtract              Q3__VectorSubtract
#define AddPointToBounds             Q3_AddPointToBounds
#define AngleDelta                   Q3_AngleDelta
#define AngleMod                     Q3_AngleMod
#define AngleNormalize180            Q3_AngleNormalize180
#define AngleNormalize360            Q3_AngleNormalize360
#define AnglesSubtract               Q3_AnglesSubtract
#define AnglesToAxis                 Q3_AnglesToAxis
#define AngleSubtract                Q3_AngleSubtract
#define AngleVectors                 Q3_AngleVectors
#define AxisClear                    Q3_AxisClear
#define AxisCopy                     Q3_AxisCopy
#define axisDefault                  Q3_axisDefault
#define BoundsIntersect              Q3_BoundsIntersect
#define BoundsIntersectPoint         Q3_BoundsIntersectPoint
#define BoundsIntersectSphere        Q3_BoundsIntersectSphere
#define BoxOnPlaneSide               Q3_BoxOnPlaneSide
#define bytedirs                     Q3_bytedirs
#define ByteToDir                    Q3_ByteToDir
#define ClampChar                    Q3_ClampChar
#define ClampShort                   Q3_ClampShort
#define ClearBounds                  Q3_ClearBounds
#define colorBlack                   Q3_colorBlack
#define colorBlue                    Q3_colorBlue
#define ColorBytes3                  Q3_ColorBytes3
#define ColorBytes4                  Q3_ColorBytes4
#define colorCyan                    Q3_colorCyan
#define colorDkGrey                  Q3_colorDkGrey
#define colorGreen                   Q3_colorGreen
#define colorLtGrey                  Q3_colorLtGrey
#define colorMagenta                 Q3_colorMagenta
#define colorMdGrey                  Q3_colorMdGrey
#define colorRed                     Q3_colorRed
#define colorWhite                   Q3_colorWhite
#define colorYellow                  Q3_colorYellow
#define COM_BeginParseSession        Q3_COM_BeginParseSession
#define Com_Clamp                    Q3_Com_Clamp
// the game DLL's shared code has its own Com_Error, and Com_Printf is a
// macro there: Quake III's are ai_q2bridge.c's
#define Com_Error                    Q3_Com_Error
#define Com_Printf                   Q3_Com_Printf
#define COM_CompareExtension         Q3_COM_CompareExtension
#define COM_Compress                 Q3_COM_Compress
#define COM_DefaultExtension         Q3_COM_DefaultExtension
#define COM_GetCurrentParseLine      Q3_COM_GetCurrentParseLine
#define COM_GetExtension             Q3_COM_GetExtension
#define Com_HexStrToInt              Q3_Com_HexStrToInt
#define COM_MatchToken               Q3_COM_MatchToken
#define COM_Parse                    Q3_COM_Parse
#define COM_ParseError               Q3_COM_ParseError
#define COM_ParseExt                 Q3_COM_ParseExt
#define COM_ParseWarning             Q3_COM_ParseWarning
#define Com_SkipCharset              Q3_Com_SkipCharset
#define COM_SkipPath                 Q3_COM_SkipPath
#define Com_SkipTokens               Q3_Com_SkipTokens
#define Com_sprintf                  Q3_Com_sprintf
#define COM_StripExtension           Q3_COM_StripExtension
#define Com_TruncateLongString       Q3_Com_TruncateLongString
#define CopyLongSwap                 Q3_CopyLongSwap
#define CopyShortSwap                Q3_CopyShortSwap
#define DirToByte                    Q3_DirToByte
#define FloatNoSwap                  Q3_FloatNoSwap
#define FloatSwap                    Q3_FloatSwap
#define g_color_table                Q3_g_color_table
#define Info_NextPair                Q3_Info_NextPair
#define Info_RemoveKey               Q3_Info_RemoveKey
#define Info_RemoveKey_Big           Q3_Info_RemoveKey_Big
#define Info_SetValueForKey          Q3_Info_SetValueForKey
#define Info_SetValueForKey_Big      Q3_Info_SetValueForKey_Big
#define Info_Validate                Q3_Info_Validate
#define Info_ValueForKey             Q3_Info_ValueForKey
#define LerpAngle                    Q3_LerpAngle
#define Long64NoSwap                 Q3_Long64NoSwap
#define Long64Swap                   Q3_Long64Swap
#define LongNoSwap                   Q3_LongNoSwap
#define LongSwap                     Q3_LongSwap
#define MakeNormalVectors            Q3_MakeNormalVectors
#define MatrixMultiply               Q3_MatrixMultiply
#define NormalizeColor               Q3_NormalizeColor
#define Parse1DMatrix                Q3_Parse1DMatrix
#define Parse2DMatrix                Q3_Parse2DMatrix
#define Parse3DMatrix                Q3_Parse3DMatrix
#define PerpendicularVector          Q3_PerpendicularVector
#define PlaneFromPoints              Q3_PlaneFromPoints
#define ProjectPointOnPlane          Q3_ProjectPointOnPlane
#define Q_acos                       Q3_Q_acos
#define Q_CleanStr                   Q3_Q_CleanStr
#define Q_CountChar                  Q3_Q_CountChar
#define Q_crandom                    Q3_Q_crandom
#define Q_fabs                       Q3_Q_fabs
#define Q_isalpha                    Q3_Q_isalpha
#define Q_isanumber                  Q3_Q_isanumber
#define Q_IsColorString              Q3_Q_IsColorString
#define Q_isintegral                 Q3_Q_isintegral
#define Q_islower                    Q3_Q_islower
#define Q_isnan                      Q3_Q_isnan
#define Q_isprint                    Q3_Q_isprint
#define Q_isupper                    Q3_Q_isupper
#define Q_log2                       Q3_Q_log2
#define Q_PrintStrlen                Q3_Q_PrintStrlen
#define Q_rand                       Q3_Q_rand
#define Q_random                     Q3_Q_random
#define Q_rsqrt                      Q3_Q_rsqrt
#define Q_strcat                     Q3_Q_strcat
#define Q_stricmp                    Q3_Q_stricmp
#define Q_stricmpn                   Q3_Q_stricmpn
#define Q_stristr                    Q3_Q_stristr
#define Q_strlwr                     Q3_Q_strlwr
#define Q_strncmp                    Q3_Q_strncmp
#define Q_strncpyz                   Q3_Q_strncpyz
#define Q_strupr                     Q3_Q_strupr
#define Q_vsnprintf                  Q3_Q_vsnprintf
#define RadiusFromBounds             Q3_RadiusFromBounds
#define RotateAroundDirection        Q3_RotateAroundDirection
#define RotatePointAroundVector      Q3_RotatePointAroundVector
#define SetPlaneSignbits             Q3_SetPlaneSignbits
#define ShortNoSwap                  Q3_ShortNoSwap
#define ShortSwap                    Q3_ShortSwap
#define SkipBracedSection            Q3_SkipBracedSection
#define SkipRestOfLine               Q3_SkipRestOfLine
#define va                           Q3_va
#define vec3_origin                  Q3_vec3_origin
#define vectoangles                  Q3_vectoangles
#define Vector4Scale                 Q3_Vector4Scale
#define VectorNormalize              Q3_VectorNormalize
#define VectorNormalize2             Q3_VectorNormalize2
#define VectorRotate                 Q3_VectorRotate

#endif // Q3_RENAME_H
