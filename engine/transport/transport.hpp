#pragma once
#include "../protocol/envelope.hpp"
namespace plpt::model {
enum class LogicalStream : std::uint8_t { Control, InputKeyboard, InputPointer, Video, Messaging, Presence };
// Contrat uniquement. Aucune implémentation réseau ni chiffrement simulé.
// L'adaptateur QUIC devra authentifier l'identité de connexion hors enveloppe,
// rejeter le 0-RTT sensible, borner les files et faire appliquer les capacités.
class ITransport {
public:
    virtual ~ITransport() = default;
    virtual Result<void> send(ParticipantId, LogicalStream, const Envelope&) = 0;
    virtual Result<void> close(ParticipantId) = 0;
};
} // namespace plpt::model
