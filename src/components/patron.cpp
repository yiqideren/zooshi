// Copyright 2015 Google Inc. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "components/attributes.h"
#include "components/patron.h"
#include "components/physics.h"
#include "components/player.h"
#include "components/player_projectile.h"
#include "components/rail_denizen.h"
#include "components/services.h"
#include "components/transform.h"
#include "events/collision.h"
#include "events/parse_action.h"
#include "mathfu/glsl_mappings.h"

using mathfu::vec3;
using mathfu::quat;

namespace fpl {
namespace fpl_project {

// All of these numbers were picked for purely aesthetic reasons:
static const float kHitMinHeight = 2.0;

static const float kSplatterCount = 10;

static const float kGravity = 0.05f;
static const float kAtRestThreshold = 0.005f;
static const float kBounceFactor = 0.4f;


void PatronComponent::Init() {
  config_ = entity_manager_->GetComponent<ServicesComponent>()->config();
  event_manager_ =
      entity_manager_->GetComponent<ServicesComponent>()->event_manager();
  event_manager_->RegisterListener(EventSinkUnion_Collision, this);
}

void PatronComponent::AddFromRawData(entity::EntityRef& entity,
                                     const void* raw_data) {
  auto component_data = static_cast<const ComponentDefInstance*>(raw_data);
  assert(component_data->data_type() == ComponentDataUnion_PatronDef);
  auto patron_def = static_cast<const PatronDef*>(component_data->data());
  PatronData* patron_data = AddEntity(entity);
  patron_data->on_collision = patron_def->on_collision();
  assert(patron_def->pop_out_radius() >= patron_def->pop_in_radius());
  patron_data->pop_in_radius_squared =
      patron_def->pop_in_radius() * patron_def->pop_in_radius();
  patron_data->pop_out_radius_squared =
      patron_def->pop_out_radius() * patron_def->pop_out_radius();
  patron_data->min_lap = patron_def->min_lap();
  patron_data->max_lap = patron_def->max_lap();
}

void PatronComponent::InitEntity(entity::EntityRef& entity) { (void)entity; }

void PatronComponent::PostLoadFixup() {
  auto physics_component =
      entity_manager_->GetComponent<PhysicsComponent>();
  for (auto iter = component_data_.begin(); iter != component_data_.end();
       ++iter) {
    // Fall down along the local y-axis
    entity::EntityRef patron = iter->entity;
    TransformData* transform_data = Data<TransformData>(patron);
    PatronData* patron_data = Data<PatronData>(patron);
    vec3 spin_direction_vector =
        transform_data->orientation.Inverse() * mathfu::kAxisY3f;
    patron_data->falling_rotation =
        quat::RotateFromTo(spin_direction_vector, vec3(0.0f, 0.0f, 1.0f));
    patron_data->original_orientation = transform_data->orientation;
    transform_data->orientation =
        patron_data->original_orientation *
        quat::Slerp(quat::identity, patron_data->falling_rotation,
                    1.0f - patron_data->y);
    // Patrons that are done should not have physics enabled.
    physics_component->DisablePhysics(patron);
  }
}

void PatronComponent::UpdateAllEntities(entity::WorldTime delta_time) {
  PlayerComponent* player_component =
      entity_manager_->GetComponent<PlayerComponent>();
  entity::EntityRef raft = player_component->begin()->entity;
  TransformData* raft_transform = Data<TransformData>(raft);
  RailDenizenData* raft_rail_denizen = Data<RailDenizenData>(raft);
  int lap = raft_rail_denizen->lap;
  for (auto iter = component_data_.begin(); iter != component_data_.end();
       ++iter) {
    entity::EntityRef patron = iter->entity;
    TransformData* transform_data = Data<TransformData>(patron);
    PatronData* patron_data = Data<PatronData>(patron);
    PatronState& state = patron_data->state;

    // Determine the patron's distance from the raft.
    float raft_distance_squared =
        (transform_data->position - raft_transform->position).LengthSquared();
    if (raft_distance_squared > patron_data->pop_out_radius_squared &&
        (state == kPatronStateUpright || state == kPatronStateGettingUp)) {
      // If you are too far away, and the patron is standing up (or getting up)
      // make them fall back down.
      patron_data->state = kPatronStateFalling;
      auto physics_component =
          entity_manager_->GetComponent<PhysicsComponent>();
      physics_component->DisablePhysics(patron);
    } else if (raft_distance_squared <= patron_data->pop_in_radius_squared &&
               lap > patron_data->last_lap_fed && lap >= patron_data->min_lap &&
               lap <= patron_data->max_lap &&
               (state == kPatronStateLayingDown ||
                state == kPatronStateFalling)) {
      // If you are in range, and the patron is standing laying down (or falling
      // down) and they have not been fed this lap, and they are in the range of
      // laps in which they should appear, make them stand back up.
      patron_data->state = kPatronStateGettingUp;
    }

    // For our simple simulation of falling and bouncing, Y is always
    // guaranteed to be between 0 and 1.
    float seconds = static_cast<float>(delta_time) / kMillisecondsPerSecond;
    if (patron_data->state == kPatronStateFalling) {
      // Some basic math to fake a convincing fall + bounce on a hinge joint.
      patron_data->dy -= seconds * kGravity;
      patron_data->y += patron_data->dy;
      if (patron_data->y < 0.0f) {
        patron_data->dy *= -kBounceFactor;
        patron_data->y = 0.0f;
        if (patron_data->dy < kAtRestThreshold) {
          patron_data->dy = 0.0f;
          patron_data->state = kPatronStateLayingDown;
        }
      }
      transform_data->orientation =
          patron_data->original_orientation *
          quat::Slerp(quat::identity, patron_data->falling_rotation,
                      1.0f - patron_data->y);
    } else if (patron_data->state == kPatronStateGettingUp) {
      // Like above, but 'falling' up no bouncing.
      patron_data->dy += seconds * kGravity;
      patron_data->y += patron_data->dy;
      if (patron_data->y >= 1.0f) {
        patron_data->y = 1.0f;
        patron_data->dy = 0.0f;
        patron_data->state = kPatronStateUpright;
        auto physics_component =
            entity_manager_->GetComponent<PhysicsComponent>();
        physics_component->EnablePhysics(patron);
      }
      transform_data->orientation =
          patron_data->original_orientation *
          quat::Slerp(quat::identity, patron_data->falling_rotation,
                      1.0f - patron_data->y);
    }
  }
}

void PatronComponent::OnEvent(const event::EventPayload& event_payload) {
  switch (event_payload.id()) {
    case EventSinkUnion_Collision: {
      auto* collision = event_payload.ToData<CollisionPayload>();
      if (collision->entity_a->IsRegisteredForComponent(GetComponentId())) {
        HandleCollision(collision->entity_a, collision->entity_b,
                        collision->position_a);
      } else if (collision->entity_b->IsRegisteredForComponent(
                     GetComponentId())) {
        HandleCollision(collision->entity_b, collision->entity_a,
                        collision->position_b);
      }
      break;
    }
    default: { assert(0); }
  }
}

void PatronComponent::HandleCollision(const entity::EntityRef& patron_entity,
                                      const entity::EntityRef& proj_entity,
                                      const mathfu::vec3& position) {
  // We only care about collisions with projectiles that haven't been deleted.
  PlayerProjectileData* projectile_data =
      Data<PlayerProjectileData>(proj_entity);
  if (projectile_data == nullptr || proj_entity->marked_for_deletion()) {
    return;
  }
  PatronData* patron_data = Data<PatronData>(patron_entity);
  if (patron_data->state == kPatronStateUpright) {
    // If the hit is high enough, consider it fed.
    // TODO: Replace this with something better, possibly multiple shapes.
    if (position.z() >= kHitMinHeight) {
      // TODO: Make state change an action.
      patron_data->state = kPatronStateFalling;
      PlayerComponent* player_component =
          entity_manager_->GetComponent<PlayerComponent>();
      entity::EntityRef raft = player_component->begin()->entity;

      RailDenizenData* raft_rail_denizen = Data<RailDenizenData>(raft);
      patron_data->last_lap_fed = raft_rail_denizen->lap;
      EventContext context;
      context.source_owner = projectile_data->owner;
      context.source = proj_entity;
      context.target = patron_entity;
      ParseAction(patron_data->on_collision, &context, event_manager_,
                  entity_manager_);
      // Disable physics after they have been fed
      auto physics_component =
          entity_manager_->GetComponent<PhysicsComponent>();
      physics_component->DisablePhysics(patron_entity);
    }

    // Even if you didn't hit the top, if got here, you got some
    // kind of collision, so you get a splatter.
    TransformData* proj_transform = Data<TransformData>(proj_entity);
    SpawnSplatter(proj_transform->position, kSplatterCount);
    entity_manager_->DeleteEntity(proj_entity);
  }
}

void PatronComponent::SpawnSplatter(const mathfu::vec3& position, int count) {
  for (int i = 0; i < count; i++) {
    entity::EntityRef particle = entity_manager_->CreateEntityFromData(
        config_->entity_defs()->Get(EntityDefs_kSplatterParticle));

    TransformData* transform_data =
        entity_manager_->GetComponentData<TransformData>(particle);
    PhysicsData* physics_data =
        entity_manager_->GetComponentData<PhysicsData>(particle);

    transform_data->position = position;

    physics_data->rigid_body->setLinearVelocity(
        btVector3(mathfu::RandomInRange(-3.0f, 3.0f),
                  mathfu::RandomInRange(-3.0f, 3.0f),
                  mathfu::RandomInRange(0.0f, 6.0f)));
    physics_data->rigid_body->setAngularVelocity(btVector3(
        mathfu::RandomInRange(1.0f, 2.0f), mathfu::RandomInRange(1.0f, 2.0f),
        mathfu::RandomInRange(1.0f, 2.0f)));

    auto physics_component = entity_manager_->GetComponent<PhysicsComponent>();
    physics_component->UpdatePhysicsFromTransform(particle);
  }
}

}  // fpl_project
}  // fpl
