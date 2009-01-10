/*
 * Clutter COGL
 *
 * A basic GL/GLES Abstraction/Utility Layer
 *
 * Authored By Matthew Allum  <mallum@openedhand.com>
 *
 * Copyright (C) 2008 OpenedHand
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "cogl.h"
#include "cogl-clip-stack.h"
#include "cogl-util.h"
#include "cogl-internal.h"

/* These are defined in the particular backend (float in GL vs fixed
   in GL ES) */
void _cogl_set_clip_planes (ClutterFixed x,
			    ClutterFixed y,
			    ClutterFixed width,
			    ClutterFixed height);
void _cogl_init_stencil_buffer (void);
void _cogl_add_stencil_clip (ClutterFixed x,
			     ClutterFixed y,
			     ClutterFixed width,
			     ClutterFixed height,
			     gboolean     first);
void _cogl_disable_clip_planes (void);
void _cogl_disable_stencil_buffer (void);
void _cogl_set_matrix (const ClutterFixed *matrix);

typedef struct _CoglClipStackEntry CoglClipStackEntry;

struct _CoglClipStackEntry
{
  /* If this is set then this entry clears the clip stack. This is
     used to clear the stack when drawing an FBO put to keep the
     entries so they can be restored when the FBO drawing is
     completed */
  gboolean            clear;

  /* The rectangle for this clip */
  ClutterFixed        x_offset;
  ClutterFixed        y_offset;
  ClutterFixed        width;
  ClutterFixed        height;

  /* The matrix that was current when the clip was set */
  ClutterFixed        matrix[16];
  ClutterFixed        viewport[4];
  
  /* this is used as a shortcut if the clipping region is rectangular */
  gboolean            is_rectangular;
  gint                scr_x_1;
  gint                scr_y_1;
  gint                scr_x_2;
  gint                scr_y_2;
};

static GList *cogl_clip_stack_top = NULL;
static int    cogl_clip_stack_depth = 0;

gboolean _cogl_clip_stack_scissor_rebuild()
{
  GList *node;
  gint x_1, y_1, x_2, y_2;
  ClutterFixed fviewport[4];
  gint viewport[4];
  gint i;

  /* go through all elements and check they are rectangular. Return if 
     they're not */
  for (node = cogl_clip_stack_top; node != NULL; node = node->next)
    {
      const CoglClipStackEntry *entry = (CoglClipStackEntry *) node->data;
      if (!entry->is_rectangular)
        return FALSE;
    }
  /* start off with a rectangle representing the screen, and clip it down
   * by each successive rectangle */
   cogl_get_viewport( fviewport );
   for (i=0;i<4;i++)
        viewport[i] = CLUTTER_FIXED_TO_INT(fviewport[i]);
   x_1 = viewport[0];
   y_1 = viewport[1];
   x_2 = viewport[0] + viewport[2];
   y_2 = viewport[1] + viewport[3];
   
   for (node = cogl_clip_stack_top; node != NULL; node = node->next)
    {
      const CoglClipStackEntry *entry = (CoglClipStackEntry *) node->data;
      if (entry->scr_x_1 > x_1) x_1 = entry->scr_x_1;
      if (entry->scr_x_2 < x_2) x_2 = entry->scr_x_2; 
      if (entry->scr_y_1 > y_1) y_1 = entry->scr_y_1;
      if (entry->scr_y_2 < y_2) y_2 = entry->scr_y_2;
    }
   /* clip negative values... */
   if (x_2 < x_1) x_2 = x_1;
   if (y_2 < y_1) y_2 = y_1;
   /* set scissoring */
   if ((x_2-x_1)!=viewport[2] || (y_2-y_1)!=viewport[3]) 
     {
       GE( glEnable( GL_SCISSOR_TEST ) ); 
       GE( glScissor( x_1-viewport[0], y_1-viewport[1], x_2-x_1, y_2-y_1 ) );
     }
   else
     {
       GE( glScissor( 0, 0, viewport[2], viewport[3] ) );
       GE( glDisable( GL_SCISSOR_TEST ) );
     }
     
  // disable in case stencilling/clip planes got turned on...
  _cogl_disable_clip_planes ();
  _cogl_disable_stencil_buffer ();     
        
  return TRUE;
}

static void
_cogl_clip_stack_add (const CoglClipStackEntry *entry, int depth)
{
  int has_clip_planes = cogl_features_available (COGL_FEATURE_FOUR_CLIP_PLANES);

  /* if we can do it all with scissoring, then just do that and return */
  if (_cogl_clip_stack_scissor_rebuild())
    return;

  /* If this is the first entry and we support clip planes then use
     that instead */
  if (depth == 1 && has_clip_planes)
    _cogl_set_clip_planes (entry->x_offset,
			   entry->y_offset,
			   entry->width,
			   entry->height);
  else
    _cogl_add_stencil_clip (entry->x_offset,
			    entry->y_offset,
			    entry->width,
			    entry->height,
			    depth == (has_clip_planes ? 2 : 1));
}

void
cogl_clip_set (ClutterFixed x_offset,
	       ClutterFixed y_offset,
	       ClutterFixed width,
	       ClutterFixed height)
{
  CoglClipStackEntry *entry = g_slice_new (CoglClipStackEntry);

  /* Make a new entry */
  entry->clear = FALSE;
  entry->x_offset = x_offset;
  entry->y_offset = y_offset;
  entry->width = width;
  entry->height = height;

  cogl_get_modelview_matrix (entry->matrix);
  
  
  /* now check for simple (rectangular!) case */
  /* TODO: we don't check for the case where rotated by
   * a multiple of 90 degrees */
  entry->is_rectangular = FALSE;
  if (entry->matrix[1]==0 && entry->matrix[2]==0 &&
      entry->matrix[4]==0 && entry->matrix[6]==0 &&
      entry->matrix[8]==0 && entry->matrix[9]==0)
    {  
        ClutterFixed viewport[4];
        ClutterFixed proj[16];
        ClutterVertex pta,ptb;
        
        entry->is_rectangular = TRUE;
        
        pta.x = entry->x_offset;
        pta.y = entry->y_offset;
        pta.z = 0;
        ptb.x = entry->x_offset + entry->width;
        ptb.y = entry->y_offset + entry->height;
        ptb.z = 0;        
      
        cogl_get_viewport (viewport);
        cogl_get_projection_matrix (proj);
        
        pta = cogl_util_unproject(entry->matrix, proj, viewport, pta);
        ptb = cogl_util_unproject(entry->matrix, proj, viewport, ptb);
          
        entry->scr_x_1 = CLUTTER_FIXED_TO_INT(pta.x);
        entry->scr_y_1 = CLUTTER_FIXED_TO_INT(pta.y);
        entry->scr_x_2 = CLUTTER_FIXED_TO_INT(ptb.x);
        entry->scr_y_2 = CLUTTER_FIXED_TO_INT(ptb.y);
        /* make sure _1 is < _2 for x and y */
        if (entry->scr_x_1 > entry->scr_x_2)
          {
            gint t = entry->scr_x_1;
            entry->scr_x_1 = entry->scr_x_2;
            entry->scr_x_2 = t; 
          }
        if (entry->scr_y_1 > entry->scr_y_2)
          {
            gint t = entry->scr_y_1;
            entry->scr_y_1 = entry->scr_y_2;
            entry->scr_y_2 = t; 
          }
    }
    
  /* Store it in the stack */
  cogl_clip_stack_top = g_list_prepend (cogl_clip_stack_top, entry);

  /* Add the entry to the current clip */
  _cogl_clip_stack_add (entry, ++cogl_clip_stack_depth);  
}

void
cogl_clip_unset (void)
{
  g_return_if_fail (cogl_clip_stack_top != NULL);

  /* Remove the top entry from the stack */
  g_slice_free (CoglClipStackEntry, cogl_clip_stack_top->data);
  cogl_clip_stack_top = g_list_delete_link (cogl_clip_stack_top,
					    cogl_clip_stack_top);
  cogl_clip_stack_depth--;

  /* Rebuild the clip */
  _cogl_clip_stack_rebuild (FALSE);
}

void
_cogl_clip_stack_rebuild (gboolean just_stencil)
{
  int has_clip_planes = cogl_features_available (COGL_FEATURE_FOUR_CLIP_PLANES);
  GList *node;
  int depth = 0;
  
  /* Attempt to use glScissor to do all our clipping */
  if (_cogl_clip_stack_scissor_rebuild())
    return;
  
  /* Disable clip planes if the stack is empty */
  if (has_clip_planes && cogl_clip_stack_depth < 1)
    _cogl_disable_clip_planes ();

  /* Disable the stencil buffer if there isn't enough entries */
  if (cogl_clip_stack_depth < (has_clip_planes ? 2 : 1))
    _cogl_disable_stencil_buffer ();

  /* Find the bottom of the stack */
  for (node = cogl_clip_stack_top; depth < cogl_clip_stack_depth - 1;
       node = node->next)
    depth++;

  /* Re-add every entry from the bottom of the stack up */
  depth = 1;
  for (; depth <= cogl_clip_stack_depth; node = node->prev, depth++)
    if (!just_stencil || !has_clip_planes || depth > 1)
      {
	const CoglClipStackEntry *entry = (CoglClipStackEntry *) node->data;
	cogl_push_matrix ();
	_cogl_set_matrix (entry->matrix);
	_cogl_clip_stack_add (entry, depth);
	cogl_pop_matrix ();
      }
}

void
_cogl_clip_stack_merge (void)
{
  GList *node = cogl_clip_stack_top;
  int i;

  /* Merge the current clip stack on top of whatever is in the stencil
     buffer */
  if (cogl_clip_stack_depth)
    {
      for (i = 0; i < cogl_clip_stack_depth - 1; i++)
	node = node->next;

      /* Skip the first entry if we have clipping planes */
      if (cogl_features_available (COGL_FEATURE_FOUR_CLIP_PLANES))
	node = node->prev;

      while (node)
	{
	  const CoglClipStackEntry *entry = (CoglClipStackEntry *) node->data;
	  cogl_push_matrix ();
	  _cogl_set_matrix (entry->matrix);
	  _cogl_clip_stack_add (entry, 3);
	  cogl_pop_matrix ();

	  node = node->prev;
	}
    }
}

void
cogl_clip_stack_save (void)
{
  CoglClipStackEntry *entry = g_slice_new (CoglClipStackEntry);

  /* Push an entry into the stack to mark that it should be cleared */
  entry->clear = TRUE;
  cogl_clip_stack_top = g_list_prepend (cogl_clip_stack_top, entry);

  /* Reset the depth to zero */
  cogl_clip_stack_depth = 0;

  /* Rebuilding the stack will now disabling all clipping */
  _cogl_clip_stack_rebuild (FALSE);
}

void
cogl_clip_stack_restore (void)
{
  GList *node;

  /* The top of the stack should be a clear marker */
  g_assert (cogl_clip_stack_top);
  g_assert (((CoglClipStackEntry *) cogl_clip_stack_top->data)->clear);

  /* Remove the top entry */
  g_slice_free (CoglClipStackEntry, cogl_clip_stack_top->data);
  cogl_clip_stack_top = g_list_delete_link (cogl_clip_stack_top,
					    cogl_clip_stack_top);

  /* Recalculate the depth of the stack */
  cogl_clip_stack_depth = 0;
  for (node = cogl_clip_stack_top;
       node && !((CoglClipStackEntry *) node->data)->clear;
       node = node->next)
    cogl_clip_stack_depth++;

  _cogl_clip_stack_rebuild (FALSE);
}