//-------------------------------------------------------------------------
/*
Copyright (C) 2010-2019 EDuke32 developers and contributors
Copyright (C) 2019 Nuke.YKT

This file is part of NBlood.

NBlood is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License version 2
as published by the Free Software Foundation.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.

See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
*/
//-------------------------------------------------------------------------
#pragma once

// DOS 1.21 validation baselines from clipmove/NotBlood, commit
// 4aa93cb12194a8864ff467a139cc2ca38595a1ff (source/blood/src/demo.cpp).
// Keep the recorded input count and comparison point in CDemo::Playback in sync:
// these values describe the state BEFORE the final recorded input is processed.

struct DEMOVALIDATE {
    const char *zName;
    int32_t nInputTicks;
    uint32_t wrandomseed;
    uint32_t health;
    vec3_t xyz;
    int nAutoAim;
};

static const DEMOVALIDATE gDemoValidate[] = {
    {"/validatedemos/TEST000.DEM", (int32_t)0x00000DB1, 0x68E811AD, 0x00000000, {(int32_t)0x00002239, (int32_t)0x00006A75, (int32_t)0x000029A4}, 1},
    {"/validatedemos/TEST001.DEM", (int32_t)0x000008E3, 0x898BFCFE, 0x00000000, {(int32_t)0x00001AC0, (int32_t)0x00003F49, (int32_t)0x000025A4}, 1},
    {"/validatedemos/TEST002.DEM", (int32_t)0x00000CE0, 0xCAF89634, 0x00000000, {(int32_t)0x0000C8ED, (int32_t)0x00009AF9, (int32_t)0x000159A4}, 1},
    {"/validatedemos/TEST003.DEM", (int32_t)0x00000B87, 0x4488E1A6, 0x00000000, {(int32_t)0xFFFFD54C, (int32_t)0xFFFFD41F, (int32_t)0x000019A4}, 1},
    // custom validation demos below
    {"/validatedemos/TEST004.DEM", (int32_t)0x00000C14, 0x6E8F8936, 0x00000000, {(int32_t)0x00003D02, (int32_t)0x00009F6E, (int32_t)0x000069A4}, 1},
    {"/validatedemos/TEST005.DEM", (int32_t)0x00001BF9, 0xE8B80FD0, 0x00000000, {(int32_t)0x00000162, (int32_t)0x00007AE0, (int32_t)0xFFFEFDA4}, 1},
    {"/validatedemos/TEST006.DEM", (int32_t)0x000036C8, 0xABF22136, 0x00000000, {(int32_t)0xFFFF2181, (int32_t)0x000091B1, (int32_t)0x000083CE}, 1},
    {"/validatedemos/TEST007.DEM", (int32_t)0x00004DA6, 0xB7FB11EB, 0x00000000, {(int32_t)0x0000F4DC, (int32_t)0x000053C9, (int32_t)0x000125A4}, 1},
    {"/validatedemos/TEST008.DEM", (int32_t)0x0000347B, 0x02CB1319, 0x00000BED, {(int32_t)0xFFFF8138, (int32_t)0xFFFED30A, (int32_t)0xFFFF7D50}, 1},
    {"/validatedemos/TEST009.DEM", (int32_t)0x000026AF, 0x4F91391D, 0x00000000, {(int32_t)0xFFFF3824, (int32_t)0xFFFF91BF, (int32_t)0x00007BA4}, 1},
    {"/validatedemos/TEST010.DEM", (int32_t)0x00000E96, 0xFF23FAE8, 0x00000000, {(int32_t)0xFFFF23DD, (int32_t)0xFFFF9EE0, (int32_t)0x00006DA4}, 1},
    {"/validatedemos/TEST011.DEM", (int32_t)0x00001706, 0x3A7DD0B0, 0x00000000, {(int32_t)0x0000CD98, (int32_t)0x00004C0A, (int32_t)0x00011DF5}, 1},
    {"/validatedemos/TEST012.DEM", (int32_t)0x00003D72, 0xE4A69539, 0x00000000, {(int32_t)0xFFFF8745, (int32_t)0xFFFED4C6, (int32_t)0xFFFF99A4}, 1},
    {"/validatedemos/TEST013.DEM", (int32_t)0x000028CE, 0x93096ADB, 0x0000046A, {(int32_t)0xFFFF826E, (int32_t)0xFFFED2C1, (int32_t)0xFFFF7D50}, 1},
    {"/validatedemos/TEST014.DEM", (int32_t)0x00000F77, 0xA8160C3C, 0x00000000, {(int32_t)0xFFFFC8D7, (int32_t)0xFFFF930F, (int32_t)0xFFFFA5A4}, 1},
    {"/validatedemos/TEST015.DEM", (int32_t)0x000016CB, 0xF6F9315E, 0x00000000, {(int32_t)0xFFFFF7A1, (int32_t)0x00007494, (int32_t)0xFFFE6651}, 1},
    {"/validatedemos/TEST016.DEM", (int32_t)0x0000165C, 0x45F41D2E, 0x00000000, {(int32_t)0x00001A1C, (int32_t)0x00008D3F, (int32_t)0xFFFF39A4}, 1},
    {"/validatedemos/TEST017.DEM", (int32_t)0x000033F7, 0x52634976, 0x00000640, {(int32_t)0x0000AC77, (int32_t)0x000012AB, (int32_t)0xFFFFFD50}, 1},
    {"/validatedemos/TEST018.DEM", (int32_t)0x00002D0C, 0x60DCE40A, 0x00000000, {(int32_t)0x0000645F, (int32_t)0x0000CB1B, (int32_t)0xFFFFC9A4}, 1},
    {"/validatedemos/TEST019.DEM", (int32_t)0x00000337, 0x4A0013E6, 0x00000640, {(int32_t)0xFFFF9FCC, (int32_t)0x00003E52, (int32_t)0xFFFF63D4}, 1},
    {"/validatedemos/TEST020.DEM", (int32_t)0x000009DE, 0xC0725AF1, 0x00000000, {(int32_t)0xFFFFC1CF, (int32_t)0x000064AD, (int32_t)0x000061A4}, 1},
    {"/validatedemos/TEST021.DEM", (int32_t)0x000019A4, 0x8BE81A09, 0x00000000, {(int32_t)0xFFFF161C, (int32_t)0x00004DB3, (int32_t)0xFFFFEEFE}, 1},
    {"/validatedemos/TEST022.DEM", (int32_t)0x00000EB3, 0x37E4D6D6, 0x00000000, {(int32_t)0x00004869, (int32_t)0x0000170D, (int32_t)0x000129A4}, 1},
    {"/validatedemos/TEST023.DEM", (int32_t)0x00001F68, 0x19B46535, 0x00000000, {(int32_t)0xFFFFDE10, (int32_t)0xFFFFD970, (int32_t)0xFFFFE9A4}, 1},
    {"/validatedemos/TEST024.DEM", (int32_t)0x00003C97, 0x7F3AE994, 0x00000640, {(int32_t)0xFFFFD2F2, (int32_t)0xFFFF8DD4, (int32_t)0xFFFF0950}, 1},
    {"/validatedemos/TEST025.DEM", (int32_t)0x0000182B, 0x330C60F2, 0x00000000, {(int32_t)0xFFFF94C1, (int32_t)0xFFFFAA81, (int32_t)0x000019A4}, 1},
    {"/validatedemos/TEST026.DEM", (int32_t)0x00002364, 0x37059BD3, 0x00000000, {(int32_t)0x0000456F, (int32_t)0xFFFFF8B8, (int32_t)0x000099A4}, 1},
    {"/validatedemos/TEST027.DEM", (int32_t)0x00001731, 0x250E075B, 0x00000000, {(int32_t)0x0000043F, (int32_t)0xFFFFD0C1, (int32_t)0x000119A4}, 1},
    {"/validatedemos/TEST028.DEM", (int32_t)0x00002FB4, 0xDF294819, 0x00000842, {(int32_t)0x000056A8, (int32_t)0xFFFFD628, (int32_t)0xFFFFFD9F}, 1},
    {"/validatedemos/TEST029.DEM", (int32_t)0x000017BB, 0xD184192B, 0x00000A1A, {(int32_t)0xFFFFAD5B, (int32_t)0xFFFF4573, (int32_t)0xFFFFE550}, 1},
    {"/validatedemos/TEST030.DEM", (int32_t)0x0000396B, 0x5031BC75, 0x00000000, {(int32_t)0x0000B326, (int32_t)0x000074C4, (int32_t)0x0000D9A4}, 1},
    {"/validatedemos/TEST031.DEM", (int32_t)0x000023D1, 0xC5DCAC81, 0x00000640, {(int32_t)0x00009BEE, (int32_t)0x00006CC8, (int32_t)0x00003550}, 1},
    {"/validatedemos/TEST032.DEM", (int32_t)0x000013BC, 0xCA389D8F, 0x00000000, {(int32_t)0x00009B71, (int32_t)0x000092A4, (int32_t)0x000159A4}, 1},
    {"/validatedemos/TEST033.DEM", (int32_t)0x00000BA4, 0xB06865D4, 0x00000000, {(int32_t)0x00009415, (int32_t)0x00007E5D, (int32_t)0x000159A4}, 1},
    {"/validatedemos/TEST034.DEM", (int32_t)0x000019C7, 0x7F7D0178, 0x00000640, {(int32_t)0x00009B90, (int32_t)0x00006CF1, (int32_t)0x000035E4}, 1},
    {"/validatedemos/TEST035.DEM", (int32_t)0x0000066A, 0xD7607579, 0x00000000, {(int32_t)0x0000B31C, (int32_t)0x0000B03F, (int32_t)0x000159A4}, 1},
    {"/validatedemos/TEST036.DEM", (int32_t)0x000016DC, 0x79095E8C, 0x00000000, {(int32_t)0x0000C2AB, (int32_t)0x000094C1, (int32_t)0x000059A4}, 1},
    {"/validatedemos/TEST037.DEM", (int32_t)0x00000B4A, 0xF9F76876, 0x00000000, {(int32_t)0x0000D00B, (int32_t)0x0000ACC8, (int32_t)0x000159A4}, 1},
    {"/validatedemos/TEST038.DEM", (int32_t)0x0000384B, 0x46086DB2, 0x0000032F, {(int32_t)0x000103E8, (int32_t)0xFFFFC601, (int32_t)0x000005E4}, 1}, // at this point msvc created binaries will desync for some weird reason
    {"/validatedemos/TEST039.DEM", (int32_t)0x000034D7, 0xC6E9D184, 0x00000000, {(int32_t)0x0000FA87, (int32_t)0x00006C32, (int32_t)0x000025A4}, 1},
    {"/validatedemos/TEST040.DEM", (int32_t)0x0000385B, 0x246696C4, 0x00000000, {(int32_t)0x00010BC8, (int32_t)0x00005F46, (int32_t)0xFFFFC1A4}, 1},
    {"/validatedemos/TEST041.DEM", (int32_t)0x00001BA1, 0x8377D75C, 0x00000000, {(int32_t)0x00006EBF, (int32_t)0x0000463F, (int32_t)0x00079DA4}, 1},
    {"/validatedemos/TEST042.DEM", (int32_t)0x00000A75, 0x61288D5E, 0x00000000, {(int32_t)0x0000A14F, (int32_t)0x00005D22, (int32_t)0x000069A4}, 1},
    {"/validatedemos/TEST043.DEM", (int32_t)0x000036FF, 0xD89DFDDD, 0x00000000, {(int32_t)0x00008B93, (int32_t)0x0000873F, (int32_t)0x000099A4}, 1},
    {"/validatedemos/TEST044.DEM", (int32_t)0x000012BD, 0xD53A7E17, 0x00000000, {(int32_t)0x0000BF3F, (int32_t)0x000079F1, (int32_t)0xFFFFC9A4}, 1},
    {"/validatedemos/TEST045.DEM", (int32_t)0x00000579, 0xDCE04A38, 0x00000000, {(int32_t)0x0000CA14, (int32_t)0x000081E9, (int32_t)0x000159A4}, 1},
    {"/validatedemos/TEST046.DEM", (int32_t)0x00000106, 0x363D780D, 0x00000000, {(int32_t)0xFFFFFF64, (int32_t)0x00009418, (int32_t)0xFFFFE1A4}, 1},
    {"/validatedemos/TEST047.DEM", (int32_t)0x00000B5F, 0x11F8A1D5, 0x000003BD, {(int32_t)0x00009A45, (int32_t)0x0000847F, (int32_t)0x0000BD50}, 1},
    {"/validatedemos/TEST048.DEM", (int32_t)0x00000AF3, 0x6E949D93, 0x00000000, {(int32_t)0xFFFF3468, (int32_t)0xFFFFD441, (int32_t)0x000061A4}, 1},
    {"/validatedemos/TEST049.DEM", (int32_t)0x00000C8F, 0x4E28530B, 0x00000000, {(int32_t)0xFFFFED7C, (int32_t)0x0000C8C0, (int32_t)0x000009A4}, 1},
    {"/validatedemos/TEST050.DEM", (int32_t)0x0000230B, 0xEA61B563, 0x00000000, {(int32_t)0x000029C1, (int32_t)0x00002C3F, (int32_t)0x000071A4}, 1},
    {"/validatedemos/TEST051.DEM", (int32_t)0x00002EA8, 0x19510828, 0x00000000, {(int32_t)0x00001FE4, (int32_t)0x000031D9, (int32_t)0x00000DA4}, 1},
    {"/validatedemos/TEST052.DEM", (int32_t)0x00005088, 0x04A4F873, 0x00000C80, {(int32_t)0x0000C0F4, (int32_t)0x0000B5ED, (int32_t)0xFFFF9F3F}, 1},
    {"/validatedemos/TEST053.DEM", (int32_t)0x000022BA, 0x93F42352, 0x00000000, {(int32_t)0x00001B69, (int32_t)0x000045D8, (int32_t)0x000011A4}, 1},
    {"/validatedemos/TEST054.DEM", (int32_t)0x00002C67, 0xE914FDC5, 0x00000000, {(int32_t)0xFFFF66E4, (int32_t)0x000049FF, (int32_t)0x000012E4}, 1},
    {"/validatedemos/TEST055.DEM", (int32_t)0x00001F8B, 0x1B5F1C0F, 0x00000000, {(int32_t)0xFFFFDD1F, (int32_t)0x00003A35, (int32_t)0x000059A4}, 1},
    {"/validatedemos/TEST056.DEM", (int32_t)0x00003EE5, 0xD02F860D, 0x00000000, {(int32_t)0xFFFF8824, (int32_t)0x0000543E, (int32_t)0x0000C1A4}, 1},
    {"/validatedemos/TEST057.DEM", (int32_t)0x00006979, 0xD9EC81A9, 0x00000000, {(int32_t)0xFFFF9197, (int32_t)0x000063D2, (int32_t)0x000061A4}, 1},
    {"/validatedemos/TEST058.DEM", (int32_t)0x00001C16, 0xD85F3129, 0x0000082F, {(int32_t)0xFFFFADAA, (int32_t)0xFFFF67D4, (int32_t)0xFFFFDD50}, 1},
    {"/validatedemos/TEST059.DEM", (int32_t)0x0000244B, 0x1E82B956, 0x00000000, {(int32_t)0x000000A4, (int32_t)0x00001F09, (int32_t)0x000219A4}, 1},
    {"/validatedemos/TEST060.DEM", (int32_t)0x000067CF, 0x086B69C7, 0x00000315, {(int32_t)0xFFFFA704, (int32_t)0x0000D1A8, (int32_t)0xFFFFFD50}, 1},
    {"/validatedemos/TEST061.DEM", (int32_t)0x000014BB, 0x89393200, 0x00000000, {(int32_t)0xFFFFC242, (int32_t)0xFFFF7FAD, (int32_t)0xFFFFCDA4}, 1},
    {"/validatedemos/TEST062.DEM", (int32_t)0x00002180, 0xB2F4CC77, 0x00000000, {(int32_t)0xFFFFB4AD, (int32_t)0x0000137E, (int32_t)0x000061A4}, 1},
    {"/validatedemos/TEST063.DEM", (int32_t)0x00000736, 0xB077ACC3, 0x00000000, {(int32_t)0xFFFF61FF, (int32_t)0xFFFFE7BA, (int32_t)0x000119A4}, 1},
    {"/validatedemos/TEST064.DEM", (int32_t)0x0000443E, 0x5FE50A44, 0x00000000, {(int32_t)0xFFFFF382, (int32_t)0x000086A6, (int32_t)0x000235A4}, 1},
    {"/validatedemos/TEST065.DEM", (int32_t)0x00003EBD, 0xA7A0D9C5, 0x00000247, {(int32_t)0xFFFF6B20, (int32_t)0xFFFFC641, (int32_t)0xFFFE1950}, 1},
    {"/validatedemos/TEST066.DEM", (int32_t)0x00003229, 0xAD1A9240, 0x00000000, {(int32_t)0xFFFF2809, (int32_t)0x0000A21E, (int32_t)0xFFFF59A4}, 1},
    {"/validatedemos/TEST067.DEM", (int32_t)0x0000374C, 0x19C88BDF, 0x00000615, {(int32_t)0xFFFFBC69, (int32_t)0xFFFF71B6, (int32_t)0xFFF9B97D}, 1},
    {"/validatedemos/TEST068.DEM", (int32_t)0x000045FF, 0x06B9C43C, 0x000004B9, {(int32_t)0xFFFEFF2E, (int32_t)0xFFFEE747, (int32_t)0xFFFE7950}, 1},
    {"/validatedemos/TEST069.DEM", (int32_t)0x00004B51, 0x1806DD38, 0x00000000, {(int32_t)0xFFFF430F, (int32_t)0x0000CF3F, (int32_t)0x000211A4}, 1},
    {"/validatedemos/TEST070.DEM", (int32_t)0x00001070, 0xA2B9B378, 0x00000000, {(int32_t)0x0000D06B, (int32_t)0x0001176F, (int32_t)0x000189A4}, 1},
    {"/validatedemos/TEST071.DEM", (int32_t)0x000082E6, 0xC74AB5A2, 0x0000016C, {(int32_t)0xFFFF80A0, (int32_t)0x00005BA3, (int32_t)0x00002950}, 1},
    {"/validatedemos/TEST072.DEM", (int32_t)0x000047C6, 0xA4405651, 0x00000000, {(int32_t)0x000099B6, (int32_t)0x000064BE, (int32_t)0x00006DA4}, 1},
    {"/validatedemos/TEST073.DEM", (int32_t)0x00002FD7, 0x11377903, 0x00000000, {(int32_t)0xFFFF7EC1, (int32_t)0xFFFEEF5F, (int32_t)0xFFF701A4}, 1},
    {"/validatedemos/TEST074.DEM", (int32_t)0x0000287A, 0xACAC602D, 0x00000000, {(int32_t)0x0000D43F, (int32_t)0x000092C1, (int32_t)0x00002844}, 1},
    {"/validatedemos/TEST075.DEM", (int32_t)0x000013DD, 0x78D2DFA4, 0x00000000, {(int32_t)0x00002362, (int32_t)0x00013B0D, (int32_t)0x000069A4}, 1},
    {"/validatedemos/TEST076.DEM", (int32_t)0x0000368A, 0x99FF70C3, 0x00000094, {(int32_t)0x00005624, (int32_t)0xFFFFD547, (int32_t)0x000000CE}, 1},
    {"/validatedemos/TEST077.DEM", (int32_t)0x00000837, 0x731125E0, 0x00000536, {(int32_t)0xFFFF8914, (int32_t)0xFFFF7F20, (int32_t)0x00002AEA}, 1},
    {"/validatedemos/TEST078.DEM", (int32_t)0x000004BB, 0x40CD5E4D, 0x00000640, {(int32_t)0x0001FCBB, (int32_t)0x0000412E, (int32_t)0x0004BAB4}, 1},
    {"/validatedemos/TEST079.DEM", (int32_t)0x000019E3, 0x4B34312A, 0x00000000, {(int32_t)0x000144F5, (int32_t)0x000143BE, (int32_t)0x00006DA4}, 1},
    {"/validatedemos/TEST080.DEM", (int32_t)0x00001169, 0xF488E48C, 0x00000000, {(int32_t)0x00008898, (int32_t)0xFFFF5BCD, (int32_t)0x00005DA4}, 1},
    {"/validatedemos/TEST081.DEM", (int32_t)0x00000864, 0xC181DEFE, 0x00000442, {(int32_t)0x00007927, (int32_t)0x00009CD7, (int32_t)0x00013D50}, 1},
    {"/validatedemos/TEST082.DEM", (int32_t)0x0000147B, 0x52769222, 0x000005EC, {(int32_t)0x00009BC9, (int32_t)0x00006EF9, (int32_t)0x00003550}, 1},
    {"/validatedemos/TEST083.DEM", (int32_t)0x00005D8D, 0xB9847039, 0x00000000, {(int32_t)0xFFFFC05A, (int32_t)0x0000AADA, (int32_t)0x000035A4}, 1},
    {"/validatedemos/TEST084.DEM", (int32_t)0x00004328, 0x74A65BA3, 0x0000009E, {(int32_t)0x000089F0, (int32_t)0xFFFF861C, (int32_t)0xFFFFF950}, 1},
    {"/validatedemos/TEST085.DEM", (int32_t)0x0000561D, 0x759EAFB7, 0x00000A6E, {(int32_t)0xFFFFF591, (int32_t)0x000059D9, (int32_t)0x00001DE4}, 1},
    {"/validatedemos/TEST086.DEM", (int32_t)0x00002820, 0x3701B79E, 0x00000000, {(int32_t)0x000018C5, (int32_t)0xFFFFC8C1, (int32_t)0x000019A4}, 0},
    {"/validatedemos/TEST087.DEM", (int32_t)0x000068CF, 0x0112953A, 0x00000640, {(int32_t)0x00000913, (int32_t)0x00005B0A, (int32_t)0x0001C150}, 0},
    {"/validatedemos/TEST088.DEM", (int32_t)0x000023DF, 0x4F0E5AB3, 0x00000000, {(int32_t)0xFFFF99A5, (int32_t)0xFFFFE2E1, (int32_t)0x000021A4}, 0},
    {"/validatedemos/TEST089.DEM", (int32_t)0x00005437, 0x5DEE1B13, 0x00000405, {(int32_t)0xFFFF65C1, (int32_t)0xFFFFA4A3, (int32_t)0xFFFEF550}, 1},
    {"/validatedemos/TEST090.DEM", (int32_t)0x0000258B, 0xFD21283F, 0x00000AF9, {(int32_t)0x0000A0C1, (int32_t)0x0000A10A, (int32_t)0x00020DE4}, 1},
    {"/validatedemos/TEST091.DEM", (int32_t)0x000034C5, 0x8893658D, 0x00000958, {(int32_t)0x0000AB42, (int32_t)0x00001360, (int32_t)0xFFFFFD50}, 1},
    {"/validatedemos/TEST092.DEM", (int32_t)0x00000D99, 0xF75E793F, 0x00000000, {(int32_t)0x00003C9D, (int32_t)0x0000D7BB, (int32_t)0xFFFF99A4}, 1},
    {"/validatedemos/TEST093.DEM", (int32_t)0x000075C7, 0xB1752688, 0x00000640, {(int32_t)0x00006693, (int32_t)0xFFFF226B, (int32_t)0xFFFD79E4}, 1},
    {"/validatedemos/TEST094.DEM", (int32_t)0x00005264, 0x119CF6D1, 0x00000000, {(int32_t)0x0000AE5B, (int32_t)0x00005036, (int32_t)0x000025A4}, 1},
    {"/validatedemos/TEST095.DEM", (int32_t)0x00002C7D, 0x6E7B8D97, 0x00000000, {(int32_t)0xFFFFEF48, (int32_t)0xFFFF9643, (int32_t)0x000099A4}, 1},
    {"/validatedemos/TEST096.DEM", (int32_t)0x00003647, 0xC2DE5F9B, 0x00000000, {(int32_t)0xFFFFF402, (int32_t)0x00000096, (int32_t)0x000389A4}, 1},
    {"/validatedemos/TEST097.DEM", (int32_t)0x00003902, 0x04ADDBE5, 0x00000000, {(int32_t)0x00014C09, (int32_t)0x00011CC1, (int32_t)0x000005A4}, 1}, // this demo crashes DOS v1.21 (ERROR (1344) src\ai.cpp)
    {"/validatedemos/TEST098.DEM", (int32_t)0x00003061, 0xB813201F, 0x0000043C, {(int32_t)0x0000957B, (int32_t)0x0001362C, (int32_t)0xFFFFE9E4}, 1},
    {"/validatedemos/TEST099.DEM", (int32_t)0x00000D8E, 0xCC24C186, 0x00000000, {(int32_t)0x0000143E, (int32_t)0x0000F753, (int32_t)0x0001F5A4}, 1},
    {"/validatedemos/TEST100.DEM", (int32_t)0x000050EE, 0x4A970C28, 0x00000128, {(int32_t)0x00001872, (int32_t)0x0000B208, (int32_t)0xFFFF0D50}, 1},
    {"/validatedemos/TEST101.DEM", (int32_t)0x00000DF2, 0x6059C7F7, 0x000004A4, {(int32_t)0x000017EB, (int32_t)0x000035CA, (int32_t)0xFFFFF1E4}, 1},
    {"/validatedemos/TEST102.DEM", (int32_t)0x000018C9, 0x972B0857, 0x00000053, {(int32_t)0x000016CF, (int32_t)0x00003540, (int32_t)0xFFFFF150}, 1},
    {"/validatedemos/TEST103.DEM", (int32_t)0x000005D9, 0x79F3760B, 0x00000000, {(int32_t)0x00001B3F, (int32_t)0xFFFFAC41, (int32_t)0x000041A4}, 0},
    {"/validatedemos/TEST104.DEM", (int32_t)0x00000ED3, 0xC1E67725, 0x00000000, {(int32_t)0x000029C1, (int32_t)0x000054BD, (int32_t)0x000069A4}, 0},
    {"/validatedemos/TEST105.DEM", (int32_t)0x000007A2, 0x48007932, 0x00000000, {(int32_t)0x0000267F, (int32_t)0xFFFFFC17, (int32_t)0x000041A4}, 0},
    {"/validatedemos/TEST106.DEM", (int32_t)0x000008E5, 0x195F6658, 0x00000000, {(int32_t)0x0000113F, (int32_t)0x000034C1, (int32_t)0x000019A4}, 0},
    {"/validatedemos/TEST107.DEM", (int32_t)0x000017D4, 0x5C2E2774, 0x00000000, {(int32_t)0x0000180D, (int32_t)0x000021C1, (int32_t)0xFFFFD9A4}, 0},
    {"/validatedemos/TEST108.DEM", (int32_t)0x000045C3, 0x79AE9FDD, 0x0000029D, {(int32_t)0x000011AD, (int32_t)0xFFFFB247, (int32_t)0x00002550}, 0},
    {"/validatedemos/TEST109.DEM", (int32_t)0x000038B4, 0x2197456B, 0x00000000, {(int32_t)0x0000196A, (int32_t)0xFFFF5C13, (int32_t)0xFFFFF9A4}, 1},
    {"/validatedemos/TEST110.DEM", (int32_t)0x00004ACC, 0xB2E036DC, 0x00000000, {(int32_t)0x000102C5, (int32_t)0x00004CC3, (int32_t)0x000031A4}, 1},
    {"/validatedemos/TEST111.DEM", (int32_t)0x000018A1, 0x7E4E907C, 0x00000000, {(int32_t)0x0000BDC1, (int32_t)0x0000583F, (int32_t)0x000019A4}, 1},
    {"/validatedemos/TEST112.DEM", (int32_t)0x00003F4C, 0x50D29210, 0x00000000, {(int32_t)0x0000F6A1, (int32_t)0xFFFED26B, (int32_t)0x00024DA4}, 1},
    {"/validatedemos/TEST113.DEM", (int32_t)0x000025DC, 0xCD2519A0, 0x0000001F, {(int32_t)0xFFFFFA41, (int32_t)0xFFFFD353, (int32_t)0x0000FD50}, 0},
    {"/validatedemos/TEST114.DEM", (int32_t)0x00001664, 0x62DBFA86, 0x00000000, {(int32_t)0x0000787B, (int32_t)0xFFFFDB53, (int32_t)0x000019A4}, 1},
    {"/validatedemos/TEST115.DEM", (int32_t)0x00001B5E, 0xB8CFF3D2, 0x00000000, {(int32_t)0xFFFF37FF, (int32_t)0xFFFFDB3F, (int32_t)0x0000E9A4}, 1},
    {"/validatedemos/TEST116.DEM", (int32_t)0x0000638C, 0xBB29CB40, 0x00000405, {(int32_t)0xFFFF7632, (int32_t)0x000053F7, (int32_t)0xFFFF8D50}, 1},
    {"/validatedemos/TEST117.DEM", (int32_t)0x00001549, 0xABD36DE5, 0x00000640, {(int32_t)0x00009ACB, (int32_t)0x00006CC1, (int32_t)0x00003550}, 1}, // this demo crashes DOS v1.21 (ERROR (3339) src\actor.cpp Bad Dude Failed: initial=0 type=0 NORMAL)
    {"/validatedemos/TEST118.DEM", (int32_t)0x00001B56, 0xF88C22B7, 0x00000640, {(int32_t)0x00009C57, (int32_t)0x00006F1B, (int32_t)0x00003550}, 1},
    {"/validatedemos/TEST119.DEM", (int32_t)0x00000E93, 0x499EF35B, 0x00000000, {(int32_t)0x00009AD5, (int32_t)0x000096BC, (int32_t)0x000159A4}, 1},
    {"/validatedemos/TEST120.DEM", (int32_t)0x000022A5, 0xC323F11D, 0x00000000, {(int32_t)0xFFFFAF9D, (int32_t)0xFFFF93D4, (int32_t)0x000009A4}, 1},
    {"/validatedemos/TEST121.DEM", (int32_t)0x0000349D, 0x2A8C0171, 0x00000000, {(int32_t)0xFFFFC62C, (int32_t)0xFFFFA38D, (int32_t)0x000009A4}, 1},
    {"/validatedemos/TEST122.DEM", (int32_t)0x00001205, 0xFBAFA0C9, 0x00000000, {(int32_t)0x0000399A, (int32_t)0x0000033F, (int32_t)0x0000B9A4}, 1},
    {"/validatedemos/TEST123.DEM", (int32_t)0x00002672, 0x5660A5AA, 0x00000AF6, {(int32_t)0xFFFFFAD0, (int32_t)0xFFFFD48D, (int32_t)0x0000FDE4}, 1},
    {"/validatedemos/TEST124.DEM", (int32_t)0x000015A5, 0xFC3CC30B, 0x00000000, {(int32_t)0xFFFF9DE6, (int32_t)0xFFFF907D, (int32_t)0x000009A4}, 1},
    {"/validatedemos/TEST125.DEM", (int32_t)0x000009AB, 0x68DB200E, 0x00000000, {(int32_t)0xFFFFF93C, (int32_t)0x00006C0C, (int32_t)0xFFFEFDA4}, 1},
    {"/validatedemos/TEST126.DEM", (int32_t)0x000008F7, 0xC60781A6, 0x00000000, {(int32_t)0x000026A0, (int32_t)0x00004087, (int32_t)0xFFFFC9A4}, 0},
    {"/validatedemos/TEST127.DEM", (int32_t)0x00001BD3, 0xA53BC5B3, 0x00000C21, {(int32_t)0x000017E7, (int32_t)0x000036A7, (int32_t)0xFFFFF1E4}, 0},
    {"/validatedemos/TEST128.DEM", (int32_t)0x000022CE, 0x1F4DC465, 0x00000640, {(int32_t)0x000016D6, (int32_t)0x00003526, (int32_t)0xFFFFF1E4}, 0},
    {"/validatedemos/TEST129.DEM", (int32_t)0x00000B4C, 0x4A2A6308, 0x00000000, {(int32_t)0x0000393F, (int32_t)0x00005E42, (int32_t)0x000051A4}, 0},
    {"/validatedemos/TEST130.DEM", (int32_t)0x00002B93, 0xF4B28CC5, 0x00000000, {(int32_t)0x000093EF, (int32_t)0x00001244, (int32_t)0x000119A4}, 1},
    {"/validatedemos/TEST131.DEM", (int32_t)0x0000633B, 0x169354F2, 0x0000048F, {(int32_t)0x0000BDC6, (int32_t)0x00005363, (int32_t)0x000009E4}, 1},
    {"/validatedemos/TEST132.DEM", (int32_t)0x000044A2, 0x9AB1E4C7, 0x00000640, {(int32_t)0xFFFFB6EA, (int32_t)0xFFFF87BA, (int32_t)0x00005D50}, 1},
    {"/validatedemos/TEST133.DEM", (int32_t)0x00001075, 0x090AEA76, 0x00000000, {(int32_t)0xFFFF327B, (int32_t)0x00009CEB, (int32_t)0x00003EE4}, 1},
    {"/validatedemos/TEST134.DEM", (int32_t)0x000008E1, 0xB172DEB5, 0x00000000, {(int32_t)0x00000482, (int32_t)0x000078C1, (int32_t)0xFFFF39A4}, 1},
    {"/validatedemos/TEST135.DEM", (int32_t)0x00002B13, 0xED1F723F, 0x000002FF, {(int32_t)0x0000A241, (int32_t)0x0000A067, (int32_t)0x00020D50}, 1},
    {"/validatedemos/TEST136.DEM", (int32_t)0x00003419, 0x5BD6E490, 0x00000B1B, {(int32_t)0x0000A245, (int32_t)0x00009F55, (int32_t)0x00020DE4}, 1},
    {"/validatedemos/TEST137.DEM", (int32_t)0x00004077, 0x9C07094D, 0x00000330, {(int32_t)0x00005CCD, (int32_t)0x00009154, (int32_t)0x00003D50}, 1},
    {"/validatedemos/TEST138.DEM", (int32_t)0x00002C1A, 0x0E08B278, 0x000006AC, {(int32_t)0x00005DBF, (int32_t)0x000092B4, (int32_t)0x00003D50}, 0},
    {"/validatedemos/TEST139.DEM", (int32_t)0x000005EE, 0x1E12503F, 0x00000000, {(int32_t)0xFFFFFE24, (int32_t)0xFFFFDB4E, (int32_t)0xFFFFF9A4}, 1},
    {"/validatedemos/TEST140.DEM", (int32_t)0x0000AFAA, 0x49A8C956, 0x00000479, {(int32_t)0xFFFFB6DD, (int32_t)0xFFFF8798, (int32_t)0x00005D50}, 1},
    {"/validatedemos/TEST141.DEM", (int32_t)0x00003B15, 0xBBFDA9CB, 0x00000B15, {(int32_t)0x00001A89, (int32_t)0x00001EE2, (int32_t)0x00002550}, 1},
    {"/validatedemos/TEST142.DEM", (int32_t)0x0000452E, 0x0980D36B, 0x000008DB, {(int32_t)0x0000273F, (int32_t)0x00004E87, (int32_t)0xFFFFFD50}, 1},
    {"/validatedemos/TEST143.DEM", (int32_t)0x000020B1, 0x23CF6A95, 0x00000000, {(int32_t)0x00001C53, (int32_t)0x00004932, (int32_t)0x000019A4}, 1},
};
