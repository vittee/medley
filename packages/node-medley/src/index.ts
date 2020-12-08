import { inherits } from 'util';
import { EventEmitter } from 'events';
const medley = require('bindings')('medley');

inherits(medley.Medley, EventEmitter);

exports.Medley = medley.Medley;
exports.Queue = medley.Queue;

export interface TrackInfo {
  path: string;
  preGain: number;
}

export type TrackDescriptor = string | TrackInfo;

export declare class Queue {
  get length(): number;

  add(track: TrackDescriptor): void;
}

export interface AudioLevel {
  magnitude: number;
  peak: number;
}

export interface AudioLevels {
  left: AudioLevel;
  right: AudioLevel;
}

type DeckEvent = 'loaded' | 'unloaded' | 'started' | 'finished';

export declare class Medley extends EventEmitter {
  constructor(queue: Queue);

  on(event: DeckEvent, listener: (deckIndex: number) => void): this;
  once(event: DeckEvent, listener: (deckIndex: number) => void): this;
  off(event: DeckEvent, listener: (deckIndex: number) => void): this;

  get level(): AudioLevels;

  play(): void;
  stop(): void;
}