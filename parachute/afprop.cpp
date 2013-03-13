#include <iFluid.h>
#include <airfoil.h>

static double (*getStateVel[3])(POINTER) = {getStateXvel,getStateYvel,
                                        getStateZvel};
static SURFACE *canopy_of_string_node(NODE*);
static void convert_to_point_mass(Front*,AF_PARAMS*);
static void string_curve_propagation(Front*,POINTER,CURVE*,CURVE*,double);
static void mono_curve_propagation(Front*,POINTER,CURVE*,CURVE*,double);
static void gore_curve_propagation(Front*,POINTER,CURVE*,CURVE*,double);
static void gore_point_propagate(Front*,POINTER,POINT*,POINT*,BOND*,double);
static	int arrayOfMonoHsbdry(INTERFACE*,CURVE**);
static	int arrayOfGoreHsbdry(INTERFACE*,CURVE**);
static 	int getGoreNodes(INTERFACE*,NODE**);

extern void elastic_point_propagate(
        Front *front,
        POINTER wave,
        POINT *oldp,
        POINT *newp,
        HYPER_SURF_ELEMENT *oldhse,
        HYPER_SURF         *oldhs,
        double              dt,
        double              *V)
{
	STATE *newsl,*newsr;
	STATE *sl,*sr;
	IF_PARAMS *iFparams = (IF_PARAMS*)front->extra1;
	AF_PARAMS *af_params = (AF_PARAMS*)front->extra2;
        IF_FIELD *field = iFparams->field;
	int i, dim = front->rect_grid->dim;
	double *vort = field->vort;
	double **vel = field->vel;
	double *pres = field->pres;
	COMPONENT base_comp = positive_component(oldhs);
	double pp[MAXD],pm[MAXD],nor[MAXD],h;
	double area_dens = af_params->area_dens;
	double left_nor_speed,right_nor_speed;
	double dv[MAXD];

	if (af_params->no_fluid)
	{
	    fourth_order_point_propagate(front,wave,oldp,newp,oldhse,
				oldhs,dt,V);
	    ft_assign(left_state(newp),left_state(oldp),front->sizest);
	    ft_assign(right_state(newp),right_state(oldp),front->sizest);
	    return;
	}

	FT_GetStatesAtPoint(oldp,oldhse,oldhs,(POINTER*)&sl,(POINTER*)&sr);
	newsl = (STATE*)left_state(newp);
	newsr = (STATE*)right_state(newp);

	FT_NormalAtPoint(oldp,front,nor,NO_COMP);
	h = FT_GridSizeInDir(nor,front);
	for (i = 0; i < dim; ++i)
	{
	    pm[i] = Coords(oldp)[i] - h*nor[i];
	    pp[i] = Coords(oldp)[i] + h*nor[i];
	}

        FT_IntrpStateVarAtCoords(front,base_comp-1,pm,pres,
			getStatePres,&newsl->pres,&sl->pres);
        FT_IntrpStateVarAtCoords(front,base_comp+1,pp,pres,
			getStatePres,&newsr->pres,&sr->pres);

	/* Impulse is incremented by the fluid pressure force */
	for (i = 0; i < dim; ++i)
	{
	    dv[i] = 0.0;
	    if (debugging("rigid_canopy"))
		dv[i] = 0.0;
	    else if (front->step > 5)
		dv[i] = (sl->pres - sr->pres)*nor[i]*dt/area_dens;
	    newsr->impulse[i] = newsl->impulse[i] = sl->impulse[i] + dv[i];
	    newsr->vel[i] = newsl->vel[i] = sl->vel[i];
	}

	/* Interpolating vorticity for the hyper surface point */
	if (dim == 2)
        {
            FT_IntrpStateVarAtCoords(front,base_comp-1,pm,vort,
				getStateVort,&newsl->vort,&sl->vort);
            FT_IntrpStateVarAtCoords(front,base_comp+1,pp,vort,
				getStateVort,&newsr->vort,&sr->vort);
        }
}       /* elastic_point_propagate */

/*	Given string node, the function finds the corresponding
*	canopy surface.
*/

static SURFACE *canopy_of_string_node(NODE *n)
{
	SURFACE *canopy,**s;
	CURVE *c,**curves;
	int i,nc;
	boolean canopy_found = NO;

	canopy = NULL;
	nc = FT_NumOfNodeCurves(n);
	FT_VectorMemoryAlloc((POINTER*)&curves,nc,sizeof(CURVE*));
	FT_ArrayOfNodeCurves(n,curves);

	for (i = 0; i < nc; ++i)
	{
	    c = curves[i];
	    for (s = c->pos_surfaces; s && *s; ++s)
	    {
		if (wave_type(*s) == ELASTIC_BOUNDARY)
		{
		    canopy_found = YES;
		    canopy = *s;
		    break;
		}
	    }
	   if (canopy_found) break;
	    for (s = c->neg_surfaces; s && *s; ++s)
	    {
		if (wave_type(*s) == ELASTIC_BOUNDARY)
		{
		    canopy_found = YES;
		    canopy = *s;
		    break;
		}
	    }
	}
	return (canopy_found == YES) ? canopy : NULL;
}	/* end canopy_of_string_node */

extern void airfoil_point_propagate(
        Front *front,
        POINTER wave,
        POINT *oldp,
        POINT *newp,
        HYPER_SURF_ELEMENT *oldhse,
        HYPER_SURF         *oldhs,
        double              dt,
        double              *V)
{
        if (wave_type(oldhs) == ELASTIC_BOUNDARY)
            return elastic_point_propagate(front,wave,oldp,newp,oldhse,oldhs,
                                        dt,V);
        else
            return ifluid_point_propagate(front,wave,oldp,newp,oldhse,oldhs,
                                        dt,V);
}       /* airfoil_point_propagate */

extern void fourth_order_elastic_set_propagate(
	Front           *newfr,
        double           fr_dt)
{
	INTERFACE *new_intfc = newfr->interf;
	NODE *newn;	/* new payload nodes */
	AF_NODE_EXTRA *extra;
	CURVE **new_str_curves,**new_mono_hsbdry,**new_gore_hsbdry;
	NODE **new_str_nodes,**new_gore_nodes;
	SURFACE *news;
	int i,num_str_curves,num_mono_hsbdry,num_gore_hsbdry;
	static PARACHUTE_SET new_geom_set;
	NODE **n;
	int num_gore_nodes = numOfGoreNodes(new_intfc);

	if (debugging("trace"))
	    (void) printf("Entering fourth_order_elastic_set_propagate()\n");
	for (n = new_intfc->nodes; n && *n; ++n)
	{
	    extra = (AF_NODE_EXTRA*)(*n)->extra;
	    if (extra == NULL || extra->af_node_type != LOAD_NODE) 
		continue;
	    newn = *n;
	    break;
	}

	new_geom_set.load_node = newn;

	num_str_curves = FT_NumOfNodeCurves(newn);

	if (num_str_curves != FT_NumOfNodeCurves(newn))
	{
	    (void) printf("ERROR in af_node_propagate(): "
			  "new number of string curves not equal!\n");
	    clean_up(ERROR);
	}
	FT_VectorMemoryAlloc((POINTER*)&new_str_curves,num_str_curves,
			sizeof(CURVE*));
	FT_VectorMemoryAlloc((POINTER*)&new_str_nodes,num_str_curves,
			sizeof(NODE*));
	FT_VectorMemoryAlloc((POINTER*)&new_gore_nodes,num_gore_nodes,
			sizeof(NODE*));
	FT_ArrayOfNodeCurves(newn,new_str_curves);

	new_geom_set.num_strings = num_str_curves;

	new_geom_set.string_node = new_str_nodes;

	new_geom_set.string_curves = new_str_curves;

	for (i = 0; i < num_str_curves; ++i)
	{
	    new_str_nodes[i] = (new_str_curves[i]->start == newn) ? 
			new_str_curves[i]->end : new_str_curves[i]->start;
	}
	news = canopy_of_string_node(new_str_nodes[0]);

	num_gore_nodes  = numOfGoreNodes(new_intfc);
	num_gore_hsbdry = numOfGoreHsbdry(new_intfc);
	num_mono_hsbdry = numOfMonoHsbdry(new_intfc);

	FT_VectorMemoryAlloc((POINTER*)&new_mono_hsbdry,num_mono_hsbdry,
			sizeof(CURVE*));
	num_mono_hsbdry = arrayOfMonoHsbdry(new_intfc,new_mono_hsbdry);
	FT_VectorMemoryAlloc((POINTER*)&new_gore_hsbdry,num_gore_hsbdry,
			sizeof(CURVE*));
	num_gore_hsbdry = arrayOfGoreHsbdry(new_intfc,new_gore_hsbdry);

	new_geom_set.num_mono_hsbdry = num_mono_hsbdry;
	new_geom_set.num_gore_hsbdry = num_gore_hsbdry;
	new_geom_set.num_gore_nodes = num_gore_nodes;

	new_geom_set.mono_hsbdry = new_mono_hsbdry;
	new_geom_set.gore_hsbdry = new_gore_hsbdry;
	getGoreNodes(new_intfc,new_gore_nodes);
	new_geom_set.gore_nodes = new_gore_nodes;
	new_geom_set.canopy = news;
	new_geom_set.front = newfr;

	if (debugging("para_set"))
	{
	    (void) printf("The parachute contains:\n");
	    (void) printf("%d string chord curves\n",num_str_curves);
	    (void) printf("%d canopy boundary curves\n",num_mono_hsbdry);
	    (void) printf("%d canopy gore curves\n",num_gore_hsbdry);
	}
	fourth_order_parachute_propagate(newfr,&new_geom_set);
	FT_FreeThese(5,new_str_curves,new_str_nodes,new_mono_hsbdry,
			new_gore_hsbdry,new_gore_nodes);
	
	if (debugging("trace"))
	    (void) printf("Leaving fourth_order_elastic_set_propagate()\n");
}	/* end fourth_order_elastic_set_propagate() */

static void convert_to_point_mass(
	Front *front,
	AF_PARAMS *af_params)
{
	INTERFACE *intfc;
	int num_str_pts,num_fabric_pts;
	SURFACE **s;
	int dim = Dimension(intfc);
	
	switch (dim)
	{
	case 2:
	    printf("In convert_to_point_mass(): 2D code needed!\n");
	    clean_up(ERROR);
	case 3:
	    for (s = intfc->surfaces; s && *s; ++s)
	    {
		if (wave_type(*s) == ELASTIC_BOUNDARY)
		{
		}
	    }
	}
}	/* end convert_to_point_mass */

extern void airfoil_curve_propagate(
        Front *front,
        POINTER wave,
	CURVE *oldc,
	CURVE *newc,
        double dt)
{
	int dim = front->rect_grid->dim;

	if (dim != 3) return;
	switch (hsbdry_type(oldc))
	{
	case STRING_HSBDRY:
	    return string_curve_propagation(front,wave,oldc,newc,dt);
	case MONO_COMP_HSBDRY:
	    return mono_curve_propagation(front,wave,oldc,newc,dt);
	case GORE_HSBDRY:
	    return gore_curve_propagation(front,wave,oldc,newc,dt);
	default:
	    return;
	}
}	/* end airfoil_curve_propagate */

static void string_curve_propagation(
        Front *front,
        POINTER wave,
	CURVE *oldc,
	CURVE *newc,
        double dt)
{
	BOND *oldb,*newb;
	POINT *oldp,*newp;

	oldp = oldc->start->posn;
	newp = newc->start->posn;
	ft_assign(left_state(newp),left_state(oldp),front->sizest);
	ft_assign(right_state(newp),right_state(oldp),front->sizest);

	oldp = oldc->end->posn;
	newp = newc->end->posn;
	ft_assign(left_state(newp),left_state(oldp),front->sizest);
	ft_assign(right_state(newp),right_state(oldp),front->sizest);

	for (oldb = oldc->first, newb = newc->first; oldb != oldc->last;
		oldb = oldb->next, newb = newb->next)
	{
	    oldp = oldb->end;
	    newp = newb->end;
	    ft_assign(left_state(newp),left_state(oldp),front->sizest);
	    ft_assign(right_state(newp),right_state(oldp),front->sizest);
	}
}	/* end string_curve_propagation */

static void gore_curve_propagation(
        Front *front,
        POINTER wave,
	CURVE *oldc,
	CURVE *newc,
        double dt)
{
	BOND *oldb,*newb;
	POINT *oldp,*newp;

	if (debugging("interact_curve"))
	{
	    (void) printf("Entering gore_curve_propagation()\n");
	}
	oldp = oldc->start->posn;
	newp = newc->start->posn;
	ft_assign(left_state(newp),left_state(oldp),front->sizest);
	ft_assign(right_state(newp),right_state(oldp),front->sizest);

	oldp = oldc->end->posn;
	newp = newc->end->posn;
	ft_assign(left_state(newp),left_state(oldp),front->sizest);
	ft_assign(right_state(newp),right_state(oldp),front->sizest);

	for (oldb = oldc->first, newb = newc->first; oldb != oldc->last;
		oldb = oldb->next, newb = newb->next)
	{
	    oldp = oldb->end;
	    newp = newb->end;
	    gore_point_propagate(front,wave,oldp,newp,oldb,dt);
	}
	if (debugging("interact_curve"))
	{
	    (void) printf("Leaving gore_curve_propagation()\n");
	}
}	/* end gore_curve_propagation */

static void gore_point_propagate(
	Front *front,
        POINTER wave,
	POINT *oldp,
	POINT *newp,
	BOND *oldb,
	double dt)
{
	BOND_TRI **btris;
	HYPER_SURF_ELEMENT *oldhse;
	HYPER_SURF         *oldhs;
	STATE *sl,*sr,*newsl,*newsr;
	double mag_nor,branch_nor[MAXD],nor[MAXD];
	double pm[MAXD],pp[MAXD],h;
	IF_PARAMS *iFparams = (IF_PARAMS*)front->extra1;
	AF_PARAMS *af_params = (AF_PARAMS*)front->extra2;
	IF_FIELD *field = iFparams->field;
	double **vel = field->vel;
        double *pres = field->pres;
	double area_dens = af_params->area_dens;
	double left_nor_speed,right_nor_speed,dv;
	COMPONENT base_comp;
	double V[MAXD];
	int i;

	if (af_params->no_fluid)
	{
	    for (btris = Btris(oldb); btris && *btris; ++btris)
	    {
	    	oldhse = Hyper_surf_element((*btris)->tri);
	    	oldhs = Hyper_surf((*btris)->surface);
	    }
	    fourth_order_point_propagate(front,wave,oldp,newp,oldhse,
				oldhs,dt,V);
	    ft_assign(left_state(newp),left_state(oldp),front->sizest);
	    ft_assign(right_state(newp),right_state(oldp),front->sizest);
	    return;
	}
	sl = (STATE*)left_state(oldp);		
	sr = (STATE*)right_state(oldp);
	newsl = (STATE*)left_state(newp);	
	newsr = (STATE*)right_state(newp);

	for (i = 0; i < 3; ++i) nor[i] = 0.0;
	for (btris = Btris(oldb); btris && *btris; ++btris)
	{
	    oldp->hse = oldhse = Hyper_surf_element((*btris)->tri);
	    oldp->hs = oldhs = Hyper_surf((*btris)->surface);
	    FT_NormalAtPoint(oldp,front,branch_nor,NO_COMP);
	    base_comp = positive_component(oldhs);
	    for (i = 0; i < 3; ++i) nor[i] += branch_nor[i];
	}
	mag_nor = Mag3d(nor);
	for (i = 0; i < 3; ++i) nor[i] /= mag_nor;
	h = FT_GridSizeInDir(nor,front);
	for (i = 0; i < 3; ++i)
	{
	    pm[i] = Coords(oldp)[i] - h*nor[i];
            pp[i] = Coords(oldp)[i] + h*nor[i];
	}
	FT_IntrpStateVarAtCoords(front,base_comp-1,pm,pres,
                        getStatePres,&newsl->pres,&sl->pres);
        FT_IntrpStateVarAtCoords(front,base_comp+1,pp,pres,
                        getStatePres,&newsr->pres,&sr->pres);
	for (i = 0; i < 3; ++i)
        {
            FT_IntrpStateVarAtCoords(front,base_comp-1,pm,vel[i],
                        getStateVel[i],&newsl->vel[i],&sl->vel[i]);
            FT_IntrpStateVarAtCoords(front,base_comp+1,pp,vel[i],
                        getStateVel[i],&newsr->vel[i],&sr->vel[i]);
        }
        left_nor_speed = Dot3d(newsl->vel,nor);
        right_nor_speed = Dot3d(newsr->vel,nor);
	for (i = 0; i < 3; ++i)
        {
            newsl->vel[i] -= left_nor_speed*nor[i];
            newsr->vel[i] -= right_nor_speed*nor[i];
        }
	/* Impulse is incremented by the fluid pressure force */
        for (i = 0; i < 3; ++i)
        {
	    dv = 0.0;
	    if (front->step > 5)
	    	dv = (sl->pres - sr->pres)*nor[i]*dt/area_dens;
	    if (debugging("rigid_canopy"))
	    	dv = 0.0;
	    newsr->impulse[i] = newsl->impulse[i] = sl->impulse[i] + dv;
	}
}	/* end gore_point_propagate */

extern int numOfMonoHsbdry(
	INTERFACE *intfc)
{
	CURVE **c;
	int nc = 0;
	for (c = intfc->curves; c && *c; ++c)
	{
	    if (hsbdry_type(*c) == MONO_COMP_HSBDRY) nc++;
	} 
	return nc;
}	/* end numOfMonoBdry */

extern int numOfGoreHsbdry(
	INTERFACE *intfc)
{
	CURVE **c;
	int nc = 0;
	for (c = intfc->curves; c && *c; ++c)
	{
	    if (hsbdry_type(*c) == GORE_HSBDRY) nc++;
	} 
	return nc;
}	/* end numOfMonoBdry */

static int arrayOfMonoHsbdry(
	INTERFACE *intfc,
	CURVE **mono_curves)
{
	CURVE **c;
	int nc = 0;
	for (c = intfc->curves; c && *c; ++c)
	{
	    if (hsbdry_type(*c) == MONO_COMP_HSBDRY) 
	    {
		mono_curves[nc] = *c;
		nc++;
	    }
	} 
	return nc;
}	/* end arrayOfMonoBdry */

static int arrayOfGoreHsbdry(
	INTERFACE *intfc,
	CURVE **gore_curves)
{
	CURVE **c;
	int nc = 0;
	for (c = intfc->curves; c && *c; ++c)
	{
	    if (hsbdry_type(*c) == GORE_HSBDRY) 
	    {
		gore_curves[nc] = *c;
		nc++;
	    }
	} 
	return nc;
}	/* end arrayOfGoreBdry */

extern int numOfGoreNodes(
	INTERFACE *intfc)
{
	NODE **n;
	CURVE **c;
	int num_gore_nodes = 0;
	AF_NODE_EXTRA *extra;
	boolean is_string_node;

	for (n = intfc->nodes; n && *n; ++n)
	{
	    if ((*n)->extra == NULL)
		continue;
	    is_string_node = NO;
	    for (c = (*n)->in_curves; c && *c; ++c)
		if (hsbdry_type(*c) == STRING_HSBDRY)
		    is_string_node = YES;
	    for (c = (*n)->out_curves; c && *c; ++c)
		if (hsbdry_type(*c) == STRING_HSBDRY)
		    is_string_node = YES;
	    if (is_string_node) continue;
	    extra = (AF_NODE_EXTRA*)(*n)->extra;
	    if (extra->af_node_type == GORE_NODE)
		num_gore_nodes++;
	}
	return num_gore_nodes;
}	/* numOfGoreNodes */

extern boolean is_bdry_node(
	NODE *node)
{
	CURVE **c;
	for (c = node->in_curves; c && *c; ++c)
	{
	    if (hsbdry_type(*c) == NEUMANN_HSBDRY ||
		hsbdry_type(*c) == DIRICHLET_HSBDRY ||
		hsbdry_type(*c) == SUBDOMAIN_HSBDRY) 
	    {
		return YES;
	    }
	} 
	for (c = node->out_curves; c && *c; ++c)
	{
	    if (hsbdry_type(*c) == NEUMANN_HSBDRY ||
		hsbdry_type(*c) == DIRICHLET_HSBDRY ||
		hsbdry_type(*c) == SUBDOMAIN_HSBDRY) 
	    {
		return YES;
	    }
	} 
	return NO;
}	/* is_bdry_node */

extern boolean is_gore_node(
	NODE *node)
{
	CURVE **c;
	AF_NODE_EXTRA *extra;

	if (node->extra == NULL)
	    return NO;
	for (c = node->in_curves; c && *c; ++c)
	    if (hsbdry_type(*c) == STRING_HSBDRY)
		return NO;
	for (c = node->out_curves; c && *c; ++c)
	    if (hsbdry_type(*c) == STRING_HSBDRY)
		return NO;
	extra = (AF_NODE_EXTRA*)(node)->extra;
	if (extra->af_node_type == GORE_NODE)
	    return YES;
	else 
	    return NO;
}	/* end is_gore_node */

extern boolean is_load_node(NODE *n)
{
        AF_NODE_EXTRA *af_node_extra;
        if (n->extra == NULL) return NO;
        af_node_extra = (AF_NODE_EXTRA*)n->extra;
        if (af_node_extra->af_node_type == LOAD_NODE) return YES;
        return NO;
}       /* end is_load_node */

static int getGoreNodes(
	INTERFACE *intfc,
	NODE **gore_nodes)
{
	NODE **n;
	int num_nodes = 0;

	for (n = intfc->nodes; n && *n; ++n)
	{
	    if (is_gore_node(*n))
		gore_nodes[num_nodes++] = *n;
	}
	return num_nodes;
}	/* getGoreNodes */

static void mono_curve_propagation(
        Front *front,
        POINTER wave,
	CURVE *oldc,
	CURVE *newc,
        double dt)
{
	BOND *oldb,*newb;
	POINT *oldp,*newp;
	double V[MAXD];
	BOND_TRI **btris;
	HYPER_SURF_ELEMENT *oldhse;
	HYPER_SURF         *oldhs;

	if (debugging("interact_curve"))
	{
	    (void) printf("Entering mono_curve_propagation()\n");
	}

	oldb = oldc->first;
	newb = newc->first;
	oldp = oldb->end;
	newp = newb->end;
	for (btris = Btris(oldb); btris && *btris; ++btris)
	{
	    oldp->hse = oldhse = Hyper_surf_element((*btris)->tri);
	    oldp->hs = oldhs = Hyper_surf((*btris)->surface);
	    elastic_point_propagate(front,wave,oldp,newp,oldhse,oldhs,dt,V);
	}
	for (oldb = oldc->first, newb = newc->first; oldb != NULL;
		oldb = oldb->next, newb = newb->next)
	{
	    oldp = oldb->end;
	    newp = newb->end;
	    for (btris = Btris(oldb); btris && *btris; ++btris)
	    {
	    	oldp->hse = oldhse = Hyper_surf_element((*btris)->tri);
	    	oldp->hs = oldhs = Hyper_surf((*btris)->surface);
		elastic_point_propagate(front,wave,oldp,newp,oldhse,oldhs,dt,V);
	    }
	}
	if (debugging("interact_curve"))
	{
	    (void) printf("Leaving mono_curve_propagation()\n");
	}
}	/* end mono_curve_propagation */

