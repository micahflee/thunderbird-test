
/*
Copyright (c) 2019 The Khronos Group Inc.
Use of this source code is governed by an MIT-style license that can be
found in the LICENSE.txt file.
*/


#ifdef GL_ES
precision mediump float;
#endif
void main()
{
    vec2 v;
    v.xy = 1.2; // swizzle needs two values, v.xy = vec2(1.2) is correct
}
