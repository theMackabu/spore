export class SerialInput {
  #chunks = [];
  #pos = 0;
  push(bytes) {
    if (bytes.length) this.#chunks.push(bytes);
  }
  readByte = () => {
    const chunk = this.#chunks[0];
    if (!chunk) return -1;
    const byte = chunk[this.#pos++];
    if (this.#pos === chunk.length) {
      this.#chunks.shift();
      this.#pos = 0;
    }
    return byte;
  };
}
