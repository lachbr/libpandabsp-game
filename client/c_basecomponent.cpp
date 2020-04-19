#include "c_basecomponent.h"
#include "simulationcomponent_shared.h"
#include "clientbase.h"
#include "pStatCollector.h"
#include "pStatTimer.h"

IMPLEMENT_CLASS( C_BaseComponent )

//
// Interpolation
//
// Entity has list of CInterpolatedVars mapped to their actual variable.
// 
// Each update, if the network time has changed, each interpolated var is marked as changed.
// If the value on the interpolated var has changed since the last update, the var is
// marked as needing interpolation.
//
// If the entity has any variables needing interpolation, it is added to a global list of entities
// that need interpolation.
//
// After receiving the latest server snapshot and updating all entities, all entities in the
// interpolation list will have interpolate() called on them.
//

static ConfigVariableBool cl_interp_all( "cl_interp_all", true );

static PStatCollector collect_interp_collector( "Entity:CollectInterpolateVars" );
static PStatCollector interp_collector( "Entity:Interpolate" );

static pvector<C_BaseComponent *> g_interpolationlist;
static pvector<C_BaseComponent *> g_teleportlist;

C_BaseComponent::C_BaseComponent() :
	BaseComponentShared(),
	_interpolation_list_entry( 0xFFFF ),
	_teleport_list_entry( 0xFFFF )
{
}

bool C_BaseComponent::initialize()
{
	if ( !BaseClass::initialize() )
	{
		return false;
	}

	interp_setup_mappings( get_var_mapping() );

	return true;
}

//
// Interpolation
//

void C_BaseComponent::add_to_teleport_list()
{
	if ( _teleport_list_entry == 0xFFFF )
	{
		g_teleportlist.push_back( this );
		_teleport_list_entry = g_teleportlist.size() - 1;
	}
}

void C_BaseComponent::remove_from_teleport_list()
{
	if ( _teleport_list_entry != 0xFFFF )
	{
		g_teleportlist.erase( g_teleportlist.begin() + _teleport_list_entry );
		_teleport_list_entry = 0xFFFF;
	}
}

void C_BaseComponent::add_to_interpolation_list()
{
	if ( _interpolation_list_entry == 0xFFFF )
	{
		//c_baseentity_cat.debug()
		//	<< "Adding " << get_entnum() << " to interpolation list" << std::endl;
		g_interpolationlist.push_back( this );
		_interpolation_list_entry = g_interpolationlist.size() - 1;
	}
}

void C_BaseComponent::remove_from_interpolation_list()
{
	if ( _interpolation_list_entry != 0xFFFF )
	{
		//c_baseentity_cat.debug()
		//	<< "Removing " << get_entnum() << " from interpolation list" << std::endl;
		g_interpolationlist.erase( std::find( g_interpolationlist.begin(), g_interpolationlist.end(), this ) );
		_interpolation_list_entry = 0xFFFF;
	}
}

float C_BaseComponent::get_last_changed_time( int flags )
{
	C_SimulationComponent *sim;
	_entity->get_component( sim );

	if ( flags & LATCH_SIMULATION_VAR )
	{
		if ( sim->simulation_time == 0.0 )
		{
			return cl->get_curtime();
		}
		return sim->simulation_time;
	}

	assert( 0 );
	return cl->get_curtime();
}

void C_BaseComponent::on_store_last_networked_value()
{
	PStatTimer timer( collect_interp_collector );

	size_t c = _var_map.m_Entries.size();
	for ( size_t i = 0; i < c; i++ )
	{
		VarMapEntry_t *e = &_var_map.m_Entries[i];
		IInterpolatedVar *watcher = e->watcher;

		int type = watcher->GetType();

		if ( type & EXCLUDE_AUTO_LATCH )
			continue;

		watcher->NoteLastNetworkedValue();
	}
}

void C_BaseComponent::on_latch_interpolated_vars( int flags )
{
	PStatTimer timer( collect_interp_collector );

	//c_baseentity_cat.debug()
	//	<< "OnLatchInterpolatedVars" << std::endl;
	float changetime = get_last_changed_time( flags );
	//std::cout << "network local changetime: " << changetime << std::endl;
	//std::cout << "currtime: " << CClockDelta::get_global_ptr()->get_local_network_time() << std::endl;


	bool update_last = ( flags & INTERPOLATE_OMIT_UPDATE_LAST_NETWORKED ) == 0;

	size_t c = _var_map.m_Entries.size();
	for ( size_t i = 0; i < c; i++ )
	{
		VarMapEntry_t *e = &_var_map.m_Entries[i];
		IInterpolatedVar *watcher = e->watcher;

		int type = watcher->GetType();

		if ( !( type & flags ) )
			continue;

		if ( type & EXCLUDE_AUTO_LATCH )
			continue;

		if ( watcher->NoteChanged( changetime, update_last ) )
		{
			e->m_bNeedsToInterpolate = true;
		}

	}

	if ( should_interpolate() )
		add_to_interpolation_list();
}

bool C_BaseComponent::interpolate( float curr_time )
{
	//c_baseentity_cat.debug()
	//	<< "interpolating " << get_entnum() << std::endl;

	LVector3 old_origin;
	LVector3 old_angles;

	int done;
	int retval =
		base_interpolate_1( curr_time, old_origin, old_angles, done );

	if ( done )
		remove_from_interpolation_list();

	return true;
}

int C_BaseComponent::base_interpolate_1( float &curr_time, LVector3 &old_org,
				      LVector3 &old_ang, int &done )
{
	// Don't mess with the world!!!
	done = 1;

	// These get moved to the parent position automatically

	//old_org = _origin;
	//old_ang = _angles;

	done = interp_interpolate( get_var_mapping(), curr_time );

	return done;
}

int C_BaseComponent::interp_interpolate( VarMapping_t *map, float curr_time )
{
	bool done = true;
	if ( curr_time < map->m_lastInterpolationTime )
	{
		for ( int i = 0; i < map->m_nInterpolatedEntries; i++ )
		{
			VarMapEntry_t *e = &map->m_Entries[i];
			e->m_bNeedsToInterpolate = true;
		}
	}
	map->m_lastInterpolationTime = curr_time;

	for ( int i = 0; i < map->m_nInterpolatedEntries; i++ )
	{
		VarMapEntry_t *e = &map->m_Entries[i];

		if ( !e->m_bNeedsToInterpolate )
			continue;

		IInterpolatedVar *watcher = e->watcher;
		assert( !( watcher->GetType() & EXCLUDE_AUTO_INTERPOLATE ) );

		if ( watcher->Interpolate( curr_time ) )
			e->m_bNeedsToInterpolate = false;
		else
			done = false;
	}

	return done;
}

bool C_BaseComponent::should_interpolate()
{
	return true;
}

void C_BaseComponent::interp_setup_mappings( VarMapping_t *map )
{
	if ( !map )
		return;

	size_t c = map->m_Entries.size();
	for ( size_t i = 0; i < c; i++ )
	{
		VarMapEntry_t *e = &map->m_Entries[i];
		IInterpolatedVar *watcher = e->watcher;
		void *data = e->data;
		int type = e->type;

		watcher->Setup( data, type );
		watcher->SetInterpolationAmount( get_interpolate_amount( watcher->GetType() ) );

	}
}

void C_BaseComponent::add_var( void *data, IInterpolatedVar *watcher, int type,
			    bool setup )
{
	// Only add it if it hasn't been added yet.
	bool bAddIt = true;
	for ( size_t i = 0; i < _var_map.m_Entries.size(); i++ )
	{
		if ( _var_map.m_Entries[i].watcher == watcher )
		{
			if ( ( type & EXCLUDE_AUTO_INTERPOLATE ) !=
			     ( watcher->GetType() & EXCLUDE_AUTO_INTERPOLATE ) )
			{
				// Its interpolation mode changed, so get rid of it and re-add it.
				remove_var( _var_map.m_Entries[i].data, true );
			}
			else
			{
				// They're adding something that's already there. No need to re-add it.
				bAddIt = false;
			}

			break;
		}
	}

	if ( bAddIt )
	{
		// watchers must have a debug name set
		nassertv( watcher->GetDebugName() != NULL );

		VarMapEntry_t map;
		map.data = data;
		map.watcher = watcher;
		map.type = type;
		map.m_bNeedsToInterpolate = true;
		if ( type & EXCLUDE_AUTO_INTERPOLATE )
		{
			_var_map.m_Entries.push_back( map );
		}
		else
		{
			_var_map.m_Entries.insert( _var_map.m_Entries.begin(), map );
			++_var_map.m_nInterpolatedEntries;
		}
	}

	if ( setup )
	{
		watcher->Setup( data, type );
		watcher->SetInterpolationAmount( get_interpolate_amount( watcher->GetType() ) );
	}
}

VarMapping_t *C_BaseComponent::get_var_mapping()
{
	return &_var_map;
}

void C_BaseComponent::remove_var( void *data, bool bAssert )
{
	for ( size_t i = 0; i < _var_map.m_Entries.size(); i++ )
	{
		if ( _var_map.m_Entries[i].data == data )
		{
			if ( !( _var_map.m_Entries[i].type & EXCLUDE_AUTO_INTERPOLATE ) )
				--_var_map.m_nInterpolatedEntries;

			_var_map.m_Entries.erase( _var_map.m_Entries.begin() + i );
			return;
		}
	}
}

float C_BaseComponent::get_interpolate_amount( int flags )
{
	int server_tick_multiple = 1;
	return cl->ticks_to_time( cl->time_to_ticks( get_client_interp_amount() ) + server_tick_multiple );
}

bool C_BaseComponent::is_predictable()
{
	return false;
}

void C_BaseComponent::interpolate_components()
{
	PStatTimer timer( interp_collector );

	//c_baseentity_cat.debug()
	//	<< "Interpolating " << g_interpolationlist.size() << " entities\n";

	for ( size_t i = 0; i < g_interpolationlist.size(); i++ )
	{
		C_BaseComponent *ent = g_interpolationlist[i];
		ent->interpolate( cl->get_curtime() );
		ent->post_interpolate();
	}
}
