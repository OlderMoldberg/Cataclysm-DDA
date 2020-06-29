#include "weather.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <string>
#include <vector>

#include "assign.h"
#include "avatar.h"
#include "bodypart.h"
#include "calendar.h"
#include "cata_utility.h"
#include "colony.h"
#include "coordinate_conversions.h"
#include "enums.h"
#include "game.h"
#include "game_constants.h"
#include "item.h"
#include "item_contents.h"
#include "line.h"
#include "map.h"
#include "math_defines.h"
#include "messages.h"
#include "options.h"
#include "overmap.h"
#include "overmapbuffer.h"
#include "regional_settings.h"
#include "rng.h"
#include "sounds.h"
#include "string_formatter.h"
#include "translations.h"
#include "trap.h"
#include "units.h"
#include "weather_gen.h"

static const activity_id ACT_WAIT_WEATHER( "ACT_WAIT_WEATHER" );

static const bionic_id bio_sunglasses( "bio_sunglasses" );

static const efftype_id effect_glare( "glare" );
static const efftype_id effect_sleep( "sleep" );
static const efftype_id effect_snow_glare( "snow_glare" );

static const itype_id itype_water( "water" );
static const itype_id itype_water_acid( "water_acid" );
static const itype_id itype_water_acid_weak( "water_acid_weak" );

static const trait_id trait_CEPH_VISION( "CEPH_VISION" );
static const trait_id trait_FEATHERS( "FEATHERS" );

static const std::string flag_SUN_GLASSES( "SUN_GLASSES" );

/**
 * \defgroup Weather "Weather and its implications."
 * @{
 */

weather_manager &get_weather()
{
    return g->weather;
}

static bool is_player_outside()
{
    return get_map().is_outside( point( g->u.posx(), g->u.posy() ) ) && g->get_levz() >= 0;
}

const weather_datum &weather::data( const weather_type type )
{
    const size_t i = static_cast<size_t>( type );
    if( i < weather_datums.size() ) {
        return weather_datums[i];
    }
    debugmsg( "Invalid weather requested." );
    return weather_datums[WEATHER_NULL];
}

weather_type weather::get_bad_weather()
{
    weather_type bad_weather = WEATHER_NULL;
    int current_weather = 0;
    for( const weather_datum &weather : weather_datums ) {
        current_weather++;
        if( weather.precip == precip_class::HEAVY ) {
            bad_weather = current_weather;
        }
    }
    return bad_weather;
}

int weather::get_count()
{
    return weather_datums.size();
}

void weather::load_weather_type( const JsonObject &jo )
{
    weather_datum weather_data;
    weather_data.id = jo.get_string( "id" );
    weather_data.name = jo.get_string( "name" );
    if( !assign( jo, "color", weather_data.color ) ) {
        jo.throw_error( "missing mandatory member \"color\"" );
    }
    if( !assign( jo, "map_color", weather_data.map_color ) ) {
        jo.throw_error( "missing mandatory member \"map_color\"" );
    }
    weather_data.glyph = jo.get_string( "glyph" )[0];
    weather_data.ranged_penalty = jo.get_int( "ranged_penalty" );
    weather_data.sight_penalty = jo.get_float( "sight_penalty" );
    weather_data.light_modifier = jo.get_int( "light_modifier" );
    weather_data.sound_attn = jo.get_int( "sound_attn" );
    weather_data.dangerous = jo.get_bool( "dangerous" );
    weather_data.precip = static_cast<precip_class>( jo.get_int( "precip" ) );
    weather_data.rains = jo.get_bool( "rains" );
    weather_data.acidic = jo.get_bool( "acidic" );
    for( const JsonObject weather_effect : jo.get_array( "effects" ) ) {


        std::pair<std::string, int> pair = std::make_pair( weather_effect.get_string( "name" ),
                                           weather_effect.get_int( "intensity" ) );

        static const std::map<std::string, weather_effect_fn> all_weather_effects = {
            { "wet", &weather_effect::wet_player },
            { "thunder", &weather_effect::thunder },
            { "lightning", &weather_effect::lightning },
            { "light_acid", &weather_effect::light_acid },
            { "acid", &weather_effect::acid }
        };
        const auto iter = all_weather_effects.find( pair.first );
        if( iter == all_weather_effects.end() ) {
            weather_effect.throw_error( "Invalid weather effect", "name" );
        }
        weather_data.effects.emplace_back( iter->second, pair.second );
    }
    weather_data.tiles_animation = jo.get_string( "tiles_animation", "" );
    if( jo.has_member( "weather_animation" ) ) {
        JsonObject weather_animation = jo.get_object( "weather_animation" );
        weather_animation_t animation;
        animation.factor = weather_animation.get_float( "factor" );
        if( !assign( weather_animation, "color", animation.color ) ) {
            jo.throw_error( "missing mandatory member \"color\"" );
        }
        animation.glyph = weather_animation.get_string( "glyph" )[0];
        weather_data.weather_animation = animation;
    } else {
        weather_data.weather_animation = { 0.0f, c_white, '?' };
    }
    weather_data.sound_category = jo.get_int( "sound_category", 0 );
    weather_data.sun_intensity = static_cast<sun_intensity_type>
                                 ( jo.get_int( "sun_intensity" ) );
    weather_data.requirements = {};
    if( jo.has_member( "requirements" ) ) {
        JsonObject weather_requires = jo.get_object( "requirements" );
        weather_requirements new_requires;
        new_requires.pressure_min = weather_requires.get_int( "pressure_min", INT_MIN );
        new_requires.pressure_max = weather_requires.get_int( "pressure_max", INT_MAX );

        new_requires.humidity_min = weather_requires.get_int( "humidity_min", INT_MIN );
        new_requires.humidity_max = weather_requires.get_int( "humidity_max", INT_MAX );

        new_requires.temperature_min = weather_requires.get_int( "temperature_min", INT_MIN );
        new_requires.temperature_max = weather_requires.get_int( "temperature_max", INT_MAX );

        new_requires.windpower_min = weather_requires.get_int( "windpower_min", INT_MIN );
        new_requires.windpower_max = weather_requires.get_int( "windpower_max", INT_MAX );

        new_requires.humidity_and_pressure = weather_requires.get_bool( "humidity_and_pressure", true );
        new_requires.acidic = weather_requires.get_bool( "acidic", false );

        new_requires.time = static_cast<time_requirement_type>( weather_requires.get_int( "time", 2 ) );
        new_requires.required_weathers = weather_requires.get_string_array( "required_weathers" );

        weather_data.requirements = new_requires;
    }
    weather_datums.push_back( weather_data );
}

/**
 * Glare.
 * Causes glare effect to player's eyes if they are not wearing applicable eye protection.
 * @param intensity Level of sun brighthess
 */
void glare( weather_type w )
{
    //General prepequisites for glare
    if( !is_player_outside() || !g->is_in_sunlight( g->u.pos() ) || g->u.in_sleep_state() ||
        g->u.worn_with_flag( flag_SUN_GLASSES ) ||
        g->u.has_bionic( bio_sunglasses ) ||
        g->u.is_blind() ) {
        return;
    }

    time_duration dur = 0_turns;
    const efftype_id *effect = nullptr;
    season_type season = season_of_year( calendar::turn );
    if( season == WINTER ) {
        //Winter snow glare: for both clear & sunny weather
        effect = &effect_snow_glare;
        dur = g->u.has_effect( *effect ) ? 1_turns : 2_turns;
    } else if( weather::data( w ).sun_intensity == sun_intensity_type::high ) {
        //Sun glare: only for bright sunny weather
        effect = &effect_glare;
        dur = g->u.has_effect( *effect ) ? 1_turns : 2_turns;
    }
    //apply final glare effect
    if( dur > 0_turns && effect != nullptr ) {
        //enhance/reduce by some traits
        if( g->u.has_trait( trait_CEPH_VISION ) ) {
            dur = dur * 2;
        }
        g->u.add_env_effect( *effect, bp_eyes, 2, dur );
    }
}

////// food vs weather

int incident_sunlight( weather_type wtype, const time_point &t )
{
    return std::max<float>( 0.0f, sunlight( t, false ) + weather::data( wtype ).light_modifier );
}

inline void proc_weather_sum( const weather_type wtype, weather_sum &data,
                              const time_point &t, const time_duration &tick_size )
{
    int amount = 0;
    if( weather::data( wtype ).rains ) {
        switch( weather::data( wtype ).precip ) {
            case precip_class::VERY_LIGHT:
                amount = 1 * to_turns<int>( tick_size );
                break;
            case precip_class::LIGHT:
                amount = 4 * to_turns<int>( tick_size );
                break;
            case precip_class::HEAVY:
                amount = 8 * to_turns<int>( tick_size );
                break;
            default:
                break;
        }
    }
    if( weather::data( wtype ).acidic ) {
        data.acid_amount += amount;
    } else {
        data.rain_amount += amount;
    }


    // TODO: Change this sunlight "sampling" here into a proper interpolation
    const float tick_sunlight = incident_sunlight( wtype, t );
    data.sunlight += tick_sunlight * to_turns<int>( tick_size );
}

weather_type current_weather( const tripoint &location, const time_point &t )
{
    const auto wgen = g->weather.get_cur_weather_gen();
    if( g->weather.weather_override != WEATHER_NULL ) {
        return g->weather.weather_override;
    }
    return wgen.get_weather_conditions( location, t, g->get_seed() );
}

////// Funnels.
weather_sum sum_conditions( const time_point &start, const time_point &end,
                            const tripoint &location )
{
    time_duration tick_size = 0_turns;
    weather_sum data;

    for( time_point t = start; t < end; t += tick_size ) {
        const time_duration diff = end - t;
        if( diff < 10_turns ) {
            tick_size = 1_turns;
        } else if( diff > 7_days ) {
            tick_size = 1_hours;
        } else {
            tick_size = 1_minutes;
        }

        weather_type wtype = current_weather( location, t );
        proc_weather_sum( wtype, data, t, tick_size );
        data.wind_amount += get_local_windpower( g->weather.windspeed,
                            overmap_buffer.ter( ms_to_omt_copy( location ) ),
                            location,
                            g->weather.winddirection, false ) * to_turns<int>( tick_size );
    }
    return data;
}

/**
 * Determine what a funnel has filled out of game, using funnelcontainer.bday as a starting point.
 */
void retroactively_fill_from_funnel( item &it, const trap &tr, const time_point &start,
                                     const time_point &end, const tripoint &pos )
{
    if( start > end || !tr.is_funnel() ) {
        return;
    }

    // bday == last fill check
    it.set_birthday( end );
    weather_sum data = sum_conditions( start, end, pos );

    // Technically 0.0 division is OK, but it will be cleaner without it
    if( data.rain_amount > 0 ) {
        const int rain = roll_remainder( 1.0 / tr.funnel_turns_per_charge( data.rain_amount ) );
        it.add_rain_to_container( false, rain );
        // add_msg(m_debug, "Retroactively adding %d water from turn %d to %d", rain, startturn, endturn);
    }

    if( data.acid_amount > 0 ) {
        const int acid = roll_remainder( 1.0 / tr.funnel_turns_per_charge( data.acid_amount ) );
        it.add_rain_to_container( true, acid );
    }
}

/**
 * Add charge(s) of rain to given container, possibly contaminating it.
 */
void item::add_rain_to_container( bool acid, int charges )
{
    if( charges <= 0 ) {
        return;
    }
    item ret( acid ? "water_acid" : "water", calendar::turn );
    const int capa = get_remaining_capacity_for_liquid( ret, true );
    ret.charges = std::min( charges, capa );
    if( contents.can_contain( ret ).success() ) {
        // This is easy. Just add 1 charge of the rain liquid to the container.
        if( !acid ) {
            // Funnels aren't always clean enough for water. // TODO: disinfectant squeegie->funnel
            ret.poison = one_in( 10 ) ? 1 : 0;
        }
        put_in( ret, item_pocket::pocket_type::CONTAINER );
    } else {
        static const std::set<itype_id> allowed_liquid_types{
            itype_water,
            itype_water_acid,
            itype_water_acid_weak
        };
        item *found_liq = contents.get_item_with( [&]( const item & liquid ) {
            return allowed_liquid_types.count( liquid.typeId() );
        } );
        if( found_liq == nullptr ) {
            debugmsg( "Rainwater failed to add to container" );
            return;
        }
        // The container already has a liquid.
        item &liq = *found_liq;
        int orig = liq.charges;
        int added = std::min( charges, capa );
        if( capa > 0 ) {
            liq.charges += added;
        }

        if( liq.typeId() == ret.typeId() || liq.typeId() == itype_water_acid_weak ) {
            // The container already contains this liquid or weakly acidic water.
            // Don't do anything special -- we already added liquid.
        } else {
            // The rain is different from what's in the container.
            // Turn the container's liquid into weak acid with a probability
            // based on its current volume.

            // If it's raining acid and this container started with 7
            // charges of water, the liquid will now be 1/8th acid or,
            // equivalently, 1/4th weak acid (the rest being water). A
            // stochastic approach gives the liquid a 1 in 4 (or 2 in
            // liquid.charges) chance of becoming weak acid.
            const bool transmute = x_in_y( 2 * added, liq.charges );

            if( transmute ) {
                liq = item( "water_acid_weak", calendar::turn, liq.charges );
            } else if( liq.typeId() == itype_water ) {
                // The container has water, and the acid rain didn't turn it
                // into weak acid. Poison the water instead, assuming 1
                // charge of acid would act like a charge of water with poison 5.
                int total_poison = liq.poison * orig + 5 * added;
                liq.poison = total_poison / liq.charges;
                int leftover_poison = total_poison - liq.poison * liq.charges;
                if( leftover_poison > rng( 0, liq.charges ) ) {
                    liq.poison++;
                }
            }
        }
    }
}

double funnel_charges_per_turn( const double surface_area_mm2, const double rain_depth_mm_per_hour )
{
    // 1mm rain on 1m^2 == 1 liter water == 1000ml
    // 1 liter == 4 volume
    // 1 volume == 250ml: containers
    // 1 volume == 200ml: water
    // How many charges of water can we collect in a turn (usually <1.0)?
    if( rain_depth_mm_per_hour == 0.0 ) {
        return 0.0;
    }

    // Calculate once, because that part is expensive
    static const item water( "water", 0 );
    // 250ml
    static const double charge_ml = static_cast<double>( to_gram( water.weight() ) ) /
                                    water.charges;

    const double vol_mm3_per_hour = surface_area_mm2 * rain_depth_mm_per_hour;
    const double vol_mm3_per_turn = vol_mm3_per_hour / to_turns<int>( 1_hours );

    const double ml_to_mm3 = 1000;
    const double charges_per_turn = vol_mm3_per_turn / ( charge_ml * ml_to_mm3 );

    return charges_per_turn;
}

double trap::funnel_turns_per_charge( double rain_depth_mm_per_hour ) const
{
    // 1mm rain on 1m^2 == 1 liter water == 1000ml
    // 1 liter == 4 volume
    // 1 volume == 250ml: containers
    // 1 volume == 200ml: water
    // How many turns should it take for us to collect 1 charge of rainwater?
    // "..."
    if( rain_depth_mm_per_hour == 0.0 ) {
        return 0.0;
    }

    const double surface_area_mm2 = M_PI * ( funnel_radius_mm * funnel_radius_mm );
    const double charges_per_turn = funnel_charges_per_turn( surface_area_mm2, rain_depth_mm_per_hour );

    if( charges_per_turn > 0.0 ) {
        return 1.0 / charges_per_turn;
    }

    return 0.0;
}

/**
 * Main routine for filling funnels from weather effects.
 */
static void fill_funnels( int rain_depth_mm_per_hour, bool acid, const trap &tr )
{
    const double turns_per_charge = tr.funnel_turns_per_charge( rain_depth_mm_per_hour );
    map &here = get_map();
    // Give each funnel on the map a chance to collect the rain.
    const std::vector<tripoint> &funnel_locs = here.trap_locations( tr.loadid );
    for( const tripoint &loc : funnel_locs ) {
        units::volume maxcontains = 0_ml;
        if( one_in( turns_per_charge ) ) {
            // FIXME:
            //add_msg("%d mm/h %d tps %.4f: fill",int(calendar::turn),rain_depth_mm_per_hour,turns_per_charge);
            // This funnel has collected some rain! Put the rain in the largest
            // container here which is either empty or contains some mixture of
            // impure water and acid.
            map_stack items = here.i_at( loc );
            auto container = items.end();
            for( auto candidate_container = items.begin(); candidate_container != items.end();
                 ++candidate_container ) {
                if( candidate_container->is_funnel_container( maxcontains ) ) {
                    container = candidate_container;
                }
            }

            if( container != items.end() ) {
                container->add_rain_to_container( acid, 1 );
                container->set_age( 0_turns );
            }
        }
    }
}

/**
 * Fill funnels and makeshift funnels from weather effects.
 * @see fill_funnels
 */
static void fill_water_collectors( int mmPerHour, bool acid )
{
    for( auto &e : trap::get_funnels() ) {
        fill_funnels( mmPerHour, acid, *e );
    }
}

/**
 * Main routine for wet effects caused by weather.
 * Drenching the player is applied after checks against worn and held items.
 *
 * The warmth of armor is considered when determining how much drench happens per tick.
 *
 * Note that this is not the only place where drenching can happen.
 * For example, moving or swimming into water tiles will also cause drenching.
 * @see fill_water_collectors
 * @see map::decay_fields_and_scent
 * @see player::drench
 */
void weather_effect::wet_player( int amount )
{
    if( !is_player_outside() ||
        g->u.has_trait( trait_FEATHERS ) ||
        g->u.weapon.has_flag( "RAIN_PROTECT" ) ||
        ( !one_in( 50 ) && g->u.worn_with_flag( "RAINPROOF" ) ) ) {
        return;
    }
    // Coarse correction to get us back to previously intended soaking rate.
    if( !calendar::once_every( 6_seconds ) ) {
        return;
    }
    const int warmth_delay = g->u.warmth( bodypart_id( "torso" ) ) * 4 / 5 + g->u.warmth(
                                 bodypart_id( "head" ) ) / 5;
    if( rng( 0, 100 - amount + warmth_delay ) > 10 ) {
        // Thick clothing slows down (but doesn't cap) soaking
        return;
    }

    const auto &wet = g->u.body_wetness;
    const auto &capacity = g->u.drench_capacity;
    body_part_set drenched_parts{ { bodypart_str_id( "torso" ), bodypart_str_id( "arm_l" ), bodypart_str_id( "arm_r" ), bodypart_str_id( "head" ) } };
    if( wet[bp_torso] * 100 >= capacity[bp_torso] * 50 ) {
        // Once upper body is 50%+ drenched, start soaking the legs too
        drenched_parts.unify_set( { { bodypart_str_id( "leg_l" ), bodypart_str_id( "leg_r" ) } } );
    }

    g->u.drench( amount, drenched_parts, false );
}

/**
 * Thunder.
 * Flavor messages. Very wet.
 */
void weather_effect::thunder( int intensity )
{
    if( !g->u.has_effect( effect_sleep ) && !g->u.is_deaf() && one_in( intensity ) ) {
        if( g->get_levz() >= 0 ) {
            add_msg( _( "You hear a distant rumble of thunder." ) );
            sfx::play_variant_sound( "environment", "thunder_far", 80, rng( 0, 359 ) );
        } else if( one_in( std::max( roll_remainder( 2.0f * g->get_levz() /
                                     g->u.mutation_value( "hearing_modifier" ) ), 1 ) ) ) {
            add_msg( _( "You hear a rumble of thunder from above." ) );
            sfx::play_variant_sound( "environment", "thunder_far",
                                     ( 80 * g->u.mutation_value( "hearing_modifier" ) ), rng( 0, 359 ) );
        }
    }
}

/**
 * Lightning.
 * Chance of lightning illumination for the current turn when aboveground. Thunder.
 *
 * This used to manifest actual lightning on the map, causing fires and such, but since such effects
 * only manifest properly near the player due to the "reality bubble", this was causing undesired metagame tactics
 * such as players leaving their shelter for a more "expendable" area during lightning storms.
 */
void weather_effect::lightning( int intensity )
{
    if( one_in( intensity ) ) {
        if( g->get_levz() >= 0 ) {
            add_msg( _( "A flash of lightning illuminates your surroundings!" ) );
            sfx::play_variant_sound( "environment", "thunder_near", 100, rng( 0, 359 ) );
            g->weather.lightning_active = true;
        }
    } else {
        g->weather.lightning_active = false;
    }
}
/**
 * Acid drizzle.
 * Causes minor pain only.
 */
void weather_effect::light_acid( int intensity )
{
    if( calendar::once_every( time_duration::from_seconds( intensity ) ) && is_player_outside() ) {
        if( g->u.weapon.has_flag( "RAIN_PROTECT" ) && !one_in( 3 ) ) {
            add_msg( _( "Your %s protects you from the acidic drizzle." ), g->u.weapon.tname() );
        } else {
            if( g->u.worn_with_flag( "RAINPROOF" ) && !one_in( 4 ) ) {
                add_msg( _( "Your clothing protects you from the acidic drizzle." ) );
            } else {
                bool has_helmet = false;
                if( g->u.is_wearing_power_armor( &has_helmet ) && ( has_helmet || !one_in( 4 ) ) ) {
                    add_msg( _( "Your power armor protects you from the acidic drizzle." ) );
                } else {
                    add_msg( m_warning, _( "The acid rain stings, but is mostly harmless for now…" ) );
                    if( one_in( 10 ) && ( g->u.get_pain() < 10 ) ) {
                        g->u.mod_pain( 1 );
                    }
                }
            }
        }
    }
}

/**
 * Acid rain.
 * Causes major pain. Damages non acid-proof mobs. Very wet (acid).
 */
void weather_effect::acid( int intensity )
{
    if( calendar::once_every( time_duration::from_seconds( intensity ) ) && is_player_outside() ) {
        if( g->u.weapon.has_flag( "RAIN_PROTECT" ) && one_in( 4 ) ) {
            add_msg( _( "Your umbrella protects you from the acid rain." ) );
        } else {
            if( g->u.worn_with_flag( "RAINPROOF" ) && one_in( 2 ) ) {
                add_msg( _( "Your clothing protects you from the acid rain." ) );
            } else {
                bool has_helmet = false;
                if( g->u.is_wearing_power_armor( &has_helmet ) && ( has_helmet || !one_in( 2 ) ) ) {
                    add_msg( _( "Your power armor protects you from the acid rain." ) );
                } else {
                    add_msg( m_bad, _( "The acid rain burns!" ) );
                    if( one_in( 2 ) && ( g->u.get_pain() < 100 ) ) {
                        g->u.mod_pain( rng( 1, 5 ) );
                    }
                }
            }
        }
    }
}

double precip_mm_per_hour( precip_class const p )
// Precipitation rate expressed as the rainfall equivalent if all
// the precipitation were rain (rather than snow).
{
    return
        p == precip_class::VERY_LIGHT ? 0.5 :
        p == precip_class::LIGHT ? 1.5 :
        p == precip_class::HEAVY ? 3   :
        0;
}

void handle_weather_effects( weather_type const w )
{
    if( weather::data( w ).rains && weather::data( w ).precip != precip_class::NONE ) {
        fill_water_collectors( precip_mm_per_hour( weather::data( w ).precip ),
                               weather::data( w ).acidic );
        int wetness = 0;
        time_duration decay_time = 60_turns;
        if( weather::data( w ).precip == precip_class::VERY_LIGHT ) {
            wetness = 5;
            decay_time = 5_turns;
        } else if( weather::data( w ).precip == precip_class::LIGHT ) {
            wetness = 30;
            decay_time = 15_turns;
        } else if( weather::data( w ).precip == precip_class::HEAVY ) {
            decay_time = 45_turns;
            wetness = 60;
        }
        g->m.decay_fields_and_scent( decay_time );
        weather_effect::wet_player( wetness );
    }
    glare( w );
    std::vector<std::pair<weather_effect_fn, int>> weather_effects = weather::data( w ).effects;

    for( const std::pair<const weather_effect_fn, int> &effect : weather_effects ) {
        effect.first( effect.second );
    }
}

static std::string to_string( const weekdays &d )
{
    static const std::array<std::string, 7> weekday_names = {{
            translate_marker( "Sunday" ), translate_marker( "Monday" )
            translate_marker( "Tuesday" ), translate_marker( "Wednesday" )
            translate_marker( "Thursday" ), translate_marker( "Friday" )
            translate_marker( "Saturday" )
        }
    };
    static_assert( static_cast<int>( weekdays::SUNDAY ) == 0,
                   "weekday_names array is out of sync with weekdays enumeration values" );
    return _( weekday_names[ static_cast<int>( d ) ] );
}

static std::string print_time_just_hour( const time_point &p )
{
    const int hour = to_hours<int>( time_past_midnight( p ) );
    int hour_param = hour % 12;
    if( hour_param == 0 ) {
        hour_param = 12;
    }
    return string_format( hour < 12 ? _( "%d AM" ) : _( "%d PM" ), hour_param );
}

// Script from Wikipedia:
// Current time
// The current time is hour/minute Eastern Standard Time
// Local conditions
// At 8 AM in Falls City, it was sunny. The temperature was 60 degrees, the dewpoint 59,
// and the relative humidity 97%. The wind was west at 6 miles (9.7 km) an hour.
// The pressure was 30.00 inches (762 mm) and steady.
// Regional conditions
// Across eastern Nebraska, southwest Iowa, and northwest Missouri, skies ranged from
// sunny to mostly sunny. It was 60 at Beatrice, 59 at Lincoln, 59 at Nebraska City, 57 at Omaha,
// 59 at Red Oak, and 62 at St. Joseph."
// Forecast
// TODAY...MOSTLY SUNNY. HIGHS IN THE LOWER 60S. NORTHEAST WINDS 5 TO 10 MPH WITH GUSTS UP TO 25 MPH.
// TONIGHT...MOSTLY CLEAR. LOWS IN THE UPPER 30S. NORTHEAST WINDS 5 TO 10 MPH.
// MONDAY...MOSTLY SUNNY. HIGHS IN THE LOWER 60S. NORTHEAST WINDS 10 TO 15 MPH.
// MONDAY NIGHT...PARTLY CLOUDY. LOWS AROUND 40. NORTHEAST WINDS 5 TO 10 MPH.

// 0% - No mention of precipitation
// 10% - No mention of precipitation, or isolated/slight chance
// 20% - Isolated/slight chance
// 30% - (Widely) scattered/chance
// 40% or 50% - Scattered/chance
// 60% or 70% - Numerous/likely
// 80%, 90% or 100% - No additional modifiers (i.e. "showers and thunderstorms")
/**
 * Generate textual weather forecast for the specified radio tower.
 */
std::string weather_forecast( const point &abs_sm_pos )
{
    std::string weather_report;
    // Local conditions
    const auto cref = overmap_buffer.closest_city( tripoint( abs_sm_pos, 0 ) );
    const std::string city_name = cref ? cref.city->name : std::string( _( "middle of nowhere" ) );
    // Current time
    weather_report += string_format(
                          //~ %1$s: time of day, %2$s: hour of day, %3$s: city name, %4$s: weather name, %5$s: temperature value
                          _( "The current time is %1$s Eastern Standard Time.  At %2$s in %3$s, it was %4$s.  The temperature was %5$s. " ),
                          to_string_time_of_day( calendar::turn ), print_time_just_hour( calendar::turn ),
                          city_name,
                          weather::data( g->weather.weather_index ).name, print_temperature( g->weather.temperature )
                      );

    //weather_report += ", the dewpoint ???, and the relative humidity ???.  ";
    //weather_report += "The wind was <direction> at ? mi/km an hour.  ";
    //weather_report += "The pressure was ??? in/mm and steady/rising/falling.";

    // Regional conditions (simulated by choosing a random range containing the current conditions).
    // Adjusted for weather volatility based on how many weather changes are coming up.
    //weather_report += "Across <region>, skies ranged from <cloudiest> to <clearest>.  ";
    // TODO: Add fake reports for nearby cities

    // TODO: weather forecast
    // forecasting periods are divided into 12-hour periods, day (6-18) and night (19-5)
    // Accumulate percentages for each period of various weather statistics, and report that
    // (with fuzz) as the weather chances.
    // int weather_proportions[NUM_WEATHER_TYPES] = {0};
    double high = -100.0;
    double low = 100.0;
    const tripoint abs_ms_pos = tripoint( sm_to_ms_copy( abs_sm_pos ), 0 );
    // TODO: wind direction and speed
    const time_point last_hour = calendar::turn - ( calendar::turn - calendar::turn_zero ) %
                                 1_hours;
    for( int d = 0; d < 6; d++ ) {
        weather_type forecast = WEATHER_NULL;
        const auto wgen = g->weather.get_cur_weather_gen();
        for( time_point i = last_hour + d * 12_hours; i < last_hour + ( d + 1 ) * 12_hours; i += 1_hours ) {
            w_point w = wgen.get_weather( abs_ms_pos, i, g->get_seed() );
            forecast = std::max( forecast, wgen.get_weather_conditions( w ) );
            high = std::max( high, w.temperature );
            low = std::min( low, w.temperature );
        }
        std::string day;
        bool started_at_night;
        const time_point c = last_hour + 12_hours * d;
        if( d == 0 && is_night( c ) ) {
            day = _( "Tonight" );
            started_at_night = true;
        } else {
            day = _( "Today" );
            started_at_night = false;
        }
        if( d > 0 && static_cast<int>( started_at_night ) != d % 2 ) {
            day = string_format( pgettext( "Mon Night", "%s Night" ), to_string( day_of_week( c ) ) );
        } else {
            day = to_string( day_of_week( c ) );
        }
        weather_report += string_format(
                              _( "%s… %s. Highs of %s. Lows of %s. " ),
                              day, weather::data( forecast ).name,
                              print_temperature( high ), print_temperature( low )
                          );
    }
    return weather_report;
}

/**
 * Print temperature (and convert to Celsius if Celsius display is enabled.)
 */
std::string print_temperature( double fahrenheit, int decimals )
{
    const auto text = [&]( const double value ) {
        return string_format( "%.*f", decimals, value );
    };

    if( get_option<std::string>( "USE_CELSIUS" ) == "celsius" ) {
        return string_format( pgettext( "temperature in Celsius", "%sC" ),
                              text( temp_to_celsius( fahrenheit ) ) );
    } else if( get_option<std::string>( "USE_CELSIUS" ) == "kelvin" ) {
        return string_format( pgettext( "temperature in Kelvin", "%sK" ),
                              text( temp_to_kelvin( fahrenheit ) ) );
    } else {
        return string_format( pgettext( "temperature in Fahrenheit", "%sF" ), text( fahrenheit ) );
    }
}

/**
 * Print relative humidity (no conversions.)
 */
std::string print_humidity( double humidity, int decimals )
{
    const std::string ret = string_format( "%.*f", decimals, humidity );
    return string_format( pgettext( "humidity in percent", "%s%%" ), ret );
}

/**
 * Print pressure (no conversions.)
 */
std::string print_pressure( double pressure, int decimals )
{
    const std::string ret = string_format( "%.*f", decimals, pressure / 10 );
    return string_format( pgettext( "air pressure in kPa", "%s kPa" ), ret );
}

int get_local_windchill( double temperature_f, double humidity, double wind_mph )
{
    double windchill_f = 0;

    if( temperature_f < 50 ) {
        /// Model 1, cold wind chill (only valid for temps below 50F)
        /// Is also used as a standard in North America.

        // Temperature is removed at the end, because get_local_windchill is meant to calculate the difference.
        // Source : http://en.wikipedia.org/wiki/Wind_chill#North_American_and_United_Kingdom_wind_chill_index
        windchill_f = 35.74 + 0.6215 * temperature_f - 35.75 * std::pow( wind_mph,
                      0.16 ) + 0.4275 * temperature_f * std::pow( wind_mph, 0.16 ) - temperature_f;
        if( wind_mph < 3 ) {
            // This model fails when wind is less than 3 mph
            windchill_f = 0;
        }
    } else {
        /// Model 2, warm wind chill

        // Source : http://en.wikipedia.org/wiki/Wind_chill#Australian_Apparent_Temperature
        // Convert to meters per second.
        double wind_meters_per_sec = wind_mph * 0.44704;
        double temperature_c = temp_to_celsius( temperature_f );

        // Cap the vapor pressure term to 50C of extra heat, as this term
        // otherwise grows logistically to an asymptotic value of about 2e7
        // for large values of temperature. This is presumably due to the
        // model being designed for reasonable ambient temperature values,
        // rather than extremely high ones.
        double windchill_c = 0.33 * std::min<float>( 150.00, humidity / 100.00 * 6.105 *
                             std::exp( 17.27 * temperature_c / ( 237.70 + temperature_c ) ) ) - 0.70 *
                             wind_meters_per_sec - 4.00;
        // Convert to Fahrenheit, but omit the '+ 32' because we are only dealing with a piece of the felt air temperature equation.
        windchill_f = windchill_c * 9 / 5;
    }

    return std::ceil( windchill_f );
}

nc_color get_wind_color( double windpower )
{
    nc_color windcolor;
    if( windpower < 1 ) {
        windcolor = c_dark_gray;
    } else if( windpower < 3 ) {
        windcolor = c_dark_gray;
    } else if( windpower < 7 ) {
        windcolor = c_light_gray;
    } else if( windpower < 12 ) {
        windcolor = c_light_gray;
    } else if( windpower < 18 ) {
        windcolor = c_blue;
    } else if( windpower < 24 ) {
        windcolor = c_blue;
    } else if( windpower < 31 ) {
        windcolor = c_light_blue;
    } else if( windpower < 38 ) {
        windcolor = c_light_blue;
    } else if( windpower < 46 ) {
        windcolor = c_cyan;
    } else if( windpower < 54 ) {
        windcolor = c_cyan;
    } else if( windpower < 63 ) {
        windcolor = c_light_cyan;
    } else if( windpower < 72 ) {
        windcolor = c_light_cyan;
    } else if( windpower > 72 ) {
        windcolor = c_white;
    }
    return windcolor;
}

std::string get_shortdirstring( int angle )
{
    std::string dirstring;
    int dirangle = angle;
    if( dirangle <= 23 || dirangle > 338 ) {
        dirstring = _( "N" );
    } else if( dirangle <= 68 ) {
        dirstring = _( "NE" );
    } else if( dirangle <= 113 ) {
        dirstring = _( "E" );
    } else if( dirangle <= 158 ) {
        dirstring = _( "SE" );
    } else if( dirangle <= 203 ) {
        dirstring = _( "S" );
    } else if( dirangle <= 248 ) {
        dirstring = _( "SW" );
    } else if( dirangle <= 293 ) {
        dirstring = _( "W" );
    } else {
        dirstring = _( "NW" );
    }
    return dirstring;
}

std::string get_dirstring( int angle )
{
    // Convert angle to cardinal directions
    std::string dirstring;
    int dirangle = angle;
    if( dirangle <= 23 || dirangle > 338 ) {
        dirstring = _( "North" );
    } else if( dirangle <= 68 ) {
        dirstring = _( "North-East" );
    } else if( dirangle <= 113 ) {
        dirstring = _( "East" );
    } else if( dirangle <= 158 ) {
        dirstring = _( "South-East" );
    } else if( dirangle <= 203 ) {
        dirstring = _( "South" );
    } else if( dirangle <= 248 ) {
        dirstring = _( "South-West" );
    } else if( dirangle <= 293 ) {
        dirstring = _( "West" );
    } else {
        dirstring = _( "North-West" );
    }
    return dirstring;
}

std::string get_wind_arrow( int dirangle )
{
    std::string wind_arrow;
    if( dirangle < 0 || dirangle >= 360 ) {
        wind_arrow.clear();
    } else if( dirangle <= 23 || dirangle > 338 ) {
        wind_arrow = "\u21D3";
    } else if( dirangle <= 68 ) {
        wind_arrow = "\u21D9";
    } else if( dirangle <= 113 ) {
        wind_arrow = "\u21D0";
    } else if( dirangle <= 158 ) {
        wind_arrow = "\u21D6";
    } else if( dirangle <= 203 ) {
        wind_arrow = "\u21D1";
    } else if( dirangle <= 248 ) {
        wind_arrow = "\u21D7";
    } else if( dirangle <= 293 ) {
        wind_arrow = "\u21D2";
    } else {
        wind_arrow = "\u21D8";
    }
    return wind_arrow;
}

int get_local_humidity( double humidity, weather_type weather, bool sheltered )
{
    int tmphumidity = humidity;
    if( sheltered ) {
        // Norm for a house?
        tmphumidity = humidity * ( 100 - humidity ) / 100 + humidity;
    } else if( weather::data( weather ).rains &&
               weather::data( weather ).precip >= precip_class::LIGHT ) {
        tmphumidity = 100;
    }

    return tmphumidity;
}

double get_local_windpower( double windpower, const oter_id &omter, const tripoint &location,
                            const int &winddirection, bool sheltered )
{
    /**
    *  A player is sheltered if he is underground, in a car, or indoors.
    **/
    if( sheltered ) {
        return 0;
    }
    rl_vec2d windvec = convert_wind_to_coord( winddirection );
    int tmpwind = static_cast<int>( windpower );
    tripoint triblocker( location + point( windvec.x, windvec.y ) );
    // Over map terrain may modify the effect of wind.
    if( is_ot_match( "forest", omter, ot_match_type::type ) ||
        is_ot_match( "forest_water", omter, ot_match_type::type ) ) {
        tmpwind = tmpwind / 2;
    }
    if( location.z > 0 ) {
        tmpwind = tmpwind + ( location.z * std::min( 5, tmpwind ) );
    }
    // An adjacent wall will block wind
    if( is_wind_blocker( triblocker ) ) {
        tmpwind = tmpwind / 10;
    }
    return static_cast<double>( tmpwind );
}

bool is_wind_blocker( const tripoint &location )
{
    return get_map().has_flag( "BLOCK_WIND", location );
}

// Description of Wind Speed - https://en.wikipedia.org/wiki/Beaufort_scale
std::string get_wind_desc( double windpower )
{
    std::string winddesc;
    if( windpower < 1 ) {
        winddesc = _( "Calm" );
    } else if( windpower <= 3 ) {
        winddesc = _( "Light Air" );
    } else if( windpower <= 7 ) {
        winddesc = _( "Light Breeze" );
    } else if( windpower <= 12 ) {
        winddesc = _( "Gentle Breeze" );
    } else if( windpower <= 18 ) {
        winddesc = _( "Moderate Breeze" );
    } else if( windpower <= 24 ) {
        winddesc = _( "Fresh Breeze" );
    } else if( windpower <= 31 ) {
        winddesc = _( "Strong Breeze" );
    } else if( windpower <= 38 ) {
        winddesc = _( "Moderate Gale" );
    } else if( windpower <= 46 ) {
        winddesc = _( "Gale" );
    } else if( windpower <= 54 ) {
        winddesc = _( "Strong Gale" );
    } else if( windpower <= 63 ) {
        winddesc = _( "Whole Gale" );
    } else if( windpower <= 72 ) {
        winddesc = _( "Violent Storm" );
    } else if( windpower > 72 ) {
        // Anything above Whole Gale is very unlikely to happen and has no additional effects.
        winddesc = _( "Hurricane" );
    }
    return winddesc;
}

rl_vec2d convert_wind_to_coord( const int angle )
{
    static const std::array<std::pair<int, rl_vec2d>, 9> outputs = {{
            { 330, rl_vec2d( 0, -1 ) },
            { 301, rl_vec2d( -1, -1 ) },
            { 240, rl_vec2d( -1, 0 ) },
            { 211, rl_vec2d( -1, 1 ) },
            { 150, rl_vec2d( 0, 1 ) },
            { 121, rl_vec2d( 1, 1 ) },
            { 60, rl_vec2d( 1, 0 ) },
            { 31, rl_vec2d( 1, -1 ) },
            { 0, rl_vec2d( 0, -1 ) }
        }
    };
    for( const std::pair<int, rl_vec2d> &val : outputs ) {
        if( angle >= val.first ) {
            return val.second;
        }
    }
    return rl_vec2d( 0, 0 );
}

bool warm_enough_to_plant( const tripoint &pos )
{
    // semi-appropriate temperature for most plants
    return g->weather.get_temperature( pos ) >= 50;
}

weather_manager::weather_manager()
{
    lightning_active = false;
    weather_override = WEATHER_NULL;
    nextweather = calendar::before_time_starts;
    temperature = 0;
    weather_index = WEATHER_DEFAULT;
}

const weather_generator &weather_manager::get_cur_weather_gen() const
{
    const overmap &om = g->get_cur_om();
    const regional_settings &settings = om.get_settings();
    return settings.weather;
}

void weather_manager::update_weather()
{
    w_point &w = *weather_precise;
    winddirection = wind_direction_override ? *wind_direction_override : w.winddirection;
    windspeed = windspeed_override ? *windspeed_override : w.windpower;
    if( weather_index == WEATHER_NULL || calendar::turn >= nextweather ) {
        const weather_generator &weather_gen = get_cur_weather_gen();
        w = weather_gen.get_weather( g->u.global_square_location(), calendar::turn, g->get_seed() );
        weather_type old_weather = weather_index;
        weather_index = weather_override == WEATHER_NULL ?
                        weather_gen.get_weather_conditions( w )
                        : weather_override;
        if( !g->u.has_artifact_with( AEP_BAD_WEATHER ) ) {
            weather_override = WEATHER_NULL;
        }
        sfx::do_ambient();
        temperature = w.temperature;
        lightning_active = false;
        // Check weather every few turns, instead of every turn.
        // TODO: predict when the weather changes and use that time.
        nextweather = calendar::turn + 5_minutes;
        const weather_datum &wdata = weather::data( weather_index );
        if( weather_index != old_weather && wdata.dangerous &&
            g->get_levz() >= 0 && here.is_outside( g->u.pos() )
            && !g->u.has_activity( ACT_WAIT_WEATHER ) ) {
            g->cancel_activity_or_ignore_query( distraction_type::weather_change,
                                                string_format( _( "The weather changed to %s!" ), wdata.name ) );
        }

        if( weather_index != old_weather && g->u.has_activity( ACT_WAIT_WEATHER ) ) {
            g->u.assign_activity( ACT_WAIT_WEATHER, 0, 0 );
        }

        if( wdata.sight_penalty !=
            weather::data( old_weather ).sight_penalty ) {
            for( int i = -OVERMAP_DEPTH; i <= OVERMAP_HEIGHT; i++ ) {
                here.set_transparency_cache_dirty( i );
            }
        }
    }
}

const weather_datum &weather_manager::data()
{
    return weather::data( weather_index );
}

const weather_datum &weather_manager::data( weather_type type )
{
    return weather::data( type );
}

void weather_manager::set_nextweather( time_point t )
{
    nextweather = t;
    update_weather();
}

int weather_manager::get_temperature( const tripoint &location )
{
    const auto &cached = temperature_cache.find( location );
    if( cached != temperature_cache.end() ) {
        return cached->second;
    }

    // local modifier
    int temp_mod = 0;

    if( !g->new_game ) {
        temp_mod += get_heat_radiation( location, false );
        temp_mod += get_convection_temperature( location );
    }
    //underground temperature = average New England temperature = 43F/6C rounded to int
    const int temp = ( location.z < 0 ? AVERAGE_ANNUAL_TEMPERATURE : temperature ) +
                     ( g->new_game ? 0 : get_map().get_temperature( location ) + temp_mod );

    temperature_cache.emplace( std::make_pair( location, temp ) );
    return temp;
}

void weather_manager::clear_temp_cache()
{
    temperature_cache.clear();
}

///@}
