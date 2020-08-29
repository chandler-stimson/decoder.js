/* global FS, Module */

{
  let busy = false;
  const jobs = [];
  const job = function() {
    if (busy) {
      return;
    }
    if (Module._decoder) {
      const job = jobs.shift();
      if (job) {
        busy = true;

        Promise.resolve(job.arrayBuffer || fetch(job.href).then(r => r.arrayBuffer())).then(ab => {
          const binary = new Uint8Array(ab);
          FS.writeFile(job.name, binary);

          Module.cwrap('decoder', 'number', ['string'])((job.name));
          FS.unlink(job.name);

          const context = new AudioContext();
          const audioBuffer = context.createBuffer(
            Module.meta['Channels'],
            FS.stat('channel_0').size / Module.meta['Sample Size'],
            Module.meta['Sample Rate']
          );
          for (let c = 0; c < Module.meta['Channels']; c += 1) {
            const filename = 'channel_' + c;
            const ch = FS.readFile(filename);
            audioBuffer.copyToChannel(new Float32Array(ch.buffer), c);
            FS.unlink(filename);
          }

          job.resolve({
            context,
            audioBuffer,
            meta: Object.assign({}, Module.meta)
          });
        }).catch(e => {
          const msg = Module.meta['Exit Message'];
          if (msg && msg !== 'NA') {
            job.reject(Error(msg));
          }
          else {
            job.reject(e);
          }
        }).finally(() => {
          busy = false;
        });
      }
    }
    else {
      throw Error('decoder is not ready');
    }
  };
  Module.version = '0.1.1';
  Module.decode = function({name, href, arrayBuffer}) {
    return new Promise((resolve, reject) => {
      jobs.push({name, href, arrayBuffer, resolve, reject});
      job();
    });
  };
}
