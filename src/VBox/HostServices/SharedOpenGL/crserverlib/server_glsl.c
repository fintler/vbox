/* $Id$ */

/** @file
 * VBox OpenGL: GLSL related functions
 */

/*
 * Copyright (C) 2009-2012 Oracle Corporation
 *
 * This file is part of VirtualBox Open Source Edition (OSE), as
 * available from http://www.virtualbox.org. This file is free software;
 * you can redistribute it and/or modify it under the terms of the GNU
 * General Public License (GPL) as published by the Free Software
 * Foundation, in version 2 as it comes in the "COPYING" file of the
 * VirtualBox OSE distribution. VirtualBox OSE is distributed in the
 * hope that it will be useful, but WITHOUT ANY WARRANTY of any kind.
 */

#include "cr_spu.h"
#include "chromium.h"
#include "cr_error.h"
#include "cr_mem.h"
#include "cr_net.h"
#include "server_dispatch.h"
#include "server.h"

#ifdef CR_OPENGL_VERSION_2_0

void SERVER_DISPATCH_APIENTRY crServerDispatchShaderSource(GLuint shader, GLsizei count, const char ** string, const GLint * length)
{
    /*@todo?crStateShaderSource(shader...);*/
#ifdef DEBUG_misha
    GLenum err = cr_server.head_spu->dispatch_table.GetError();
#endif
    cr_server.head_spu->dispatch_table.ShaderSource(crStateGetShaderHWID(shader), count, string, length);
#ifdef DEBUG_misha
    err = cr_server.head_spu->dispatch_table.GetError();
    CRASSERT(err == GL_NO_ERROR);
#endif
    CR_SERVER_DUMP_SHADER_SOURCE(shader);
}

void SERVER_DISPATCH_APIENTRY crServerDispatchCompileShader(GLuint shader)
{
#ifdef DEBUG_misha
    GLint iCompileStatus = GL_FALSE;
#endif
    crStateCompileShader(shader);
    cr_server.head_spu->dispatch_table.CompileShader(crStateGetShaderHWID(shader));
#ifdef DEBUG_misha
    cr_server.head_spu->dispatch_table.GetShaderiv(crStateGetShaderHWID(shader), GL_COMPILE_STATUS, &iCompileStatus);
    Assert(iCompileStatus == GL_TRUE);
#endif
    CR_SERVER_DUMP_COMPILE_SHADER(shader);
}

void SERVER_DISPATCH_APIENTRY crServerDispatchDeleteShader(GLuint shader)
{
    GLuint shaderHW = crStateGetShaderHWID(shader);
    crStateDeleteShader(shader);
    if (shaderHW)
        cr_server.head_spu->dispatch_table.DeleteShader(shaderHW);
    else
        crWarning("crServerDispatchDeleteShader: hwid not found for shader(%d)", shader);
}

void SERVER_DISPATCH_APIENTRY crServerDispatchAttachShader(GLuint program, GLuint shader)
{
    crStateAttachShader(program, shader);
    cr_server.head_spu->dispatch_table.AttachShader(crStateGetProgramHWID(program), crStateGetShaderHWID(shader));
}

void SERVER_DISPATCH_APIENTRY crServerDispatchDetachShader(GLuint program, GLuint shader)
{
    crStateDetachShader(program, shader);
    cr_server.head_spu->dispatch_table.DetachShader(crStateGetProgramHWID(program), crStateGetShaderHWID(shader));
}

void SERVER_DISPATCH_APIENTRY crServerDispatchLinkProgram(GLuint program)
{
    crStateLinkProgram(program);
    cr_server.head_spu->dispatch_table.LinkProgram(crStateGetProgramHWID(program));
    CR_SERVER_DUMP_LINK_PROGRAM(program);
}

void SERVER_DISPATCH_APIENTRY crServerDispatchUseProgram(GLuint program)
{
    crStateUseProgram(program);
    cr_server.head_spu->dispatch_table.UseProgram(crStateGetProgramHWID(program));
}

void SERVER_DISPATCH_APIENTRY crServerDispatchDeleteProgram(GLuint program)
{
    GLuint hwId = crStateGetProgramHWID(program);
    crStateDeleteProgram(program);
    if (hwId)
        cr_server.head_spu->dispatch_table.DeleteProgram(hwId);
    else
        crWarning("crServerDispatchDeleteProgram: hwid not found for program(%d)", program);
}

void SERVER_DISPATCH_APIENTRY crServerDispatchValidateProgram(GLuint program)
{
    crStateValidateProgram(program);
    cr_server.head_spu->dispatch_table.ValidateProgram(crStateGetProgramHWID(program));
}

void SERVER_DISPATCH_APIENTRY crServerDispatchBindAttribLocation(GLuint program, GLuint index, const char * name)
{
    crStateBindAttribLocation(program, index, name);
    cr_server.head_spu->dispatch_table.BindAttribLocation(crStateGetProgramHWID(program), index, name);
}

void SERVER_DISPATCH_APIENTRY crServerDispatchDeleteObjectARB(GLhandleARB obj)
{
    GLuint hwid =  crStateDeleteObjectARB(obj);

    if (hwid)
        cr_server.head_spu->dispatch_table.DeleteObjectARB(hwid);
    else
        crWarning("zero hwid for object %d", obj);
}

GLint SERVER_DISPATCH_APIENTRY crServerDispatchGetAttribLocation( GLuint program, const char * name )
{
    GLint retval;
    retval = cr_server.head_spu->dispatch_table.GetAttribLocation(crStateGetProgramHWID(program), name );
    crServerReturnValue( &retval, sizeof(retval) );
    return retval; /* WILL PROBABLY BE IGNORED */
}

GLhandleARB SERVER_DISPATCH_APIENTRY crServerDispatchGetHandleARB( GLenum pname )
{
    GLhandleARB retval;
    retval = cr_server.head_spu->dispatch_table.GetHandleARB(pname);
    if (pname==GL_PROGRAM_OBJECT_ARB)
    {
        retval = crStateGLSLProgramHWIDtoID(retval);
    }
    crServerReturnValue( &retval, sizeof(retval) );
    return retval; /* WILL PROBABLY BE IGNORED */
}

GLint SERVER_DISPATCH_APIENTRY crServerDispatchGetUniformLocation(GLuint program, const char * name)
{
    GLint retval;
    retval = cr_server.head_spu->dispatch_table.GetUniformLocation(crStateGetProgramHWID(program), name);
    crServerReturnValue( &retval, sizeof(retval) );
    return retval; /* WILL PROBABLY BE IGNORED */
}

#endif /* #ifdef CR_OPENGL_VERSION_2_0 */
