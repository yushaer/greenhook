/** Globals provided by Greenhook's QuickJS runtime for a project using lib ES2020. */
declare const console: {
    clear(): void;
    debug(...values: unknown[]): void;
    log(...values: unknown[]): void;
    info(...values: unknown[]): void;
    warn(...values: unknown[]): void;
    error(...values: unknown[]): void;
};
declare class TextEncoder {
    readonly encoding: string;
    encode(text?: string): Uint8Array;
}
declare class TextDecoder {
    readonly encoding: string;
    decode(bytes?: Uint8Array | ArrayBuffer): string;
}
